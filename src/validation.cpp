// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <validation.h>

#include <zside/marker.h>
#include <zside/shielded.h>

#include <arith_uint256.h>
#include <chain.h>
#include <checkqueue.h>
#include <clientversion.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_check.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <cuckoocache.h>
#include <flatfile.h>
#include <hash.h>
#include <kernel/chainparams.h>
#include <kernel/coinstats.h>
#include <kernel/disconnected_transactions.h>
#include <kernel/mempool_entry.h>
#include <kernel/messagestartchars.h>
#include <kernel/notifications_interface.h>
#include <kernel/types.h>
#include <kernel/warning.h>
#include <logging/timer.h>
#include <node/blockstorage.h>
#include <node/utxo_snapshot.h>
#include <policy/ephemeral_policy.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <policy/settings.h>
#include <policy/truc_policy.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/sigcache.h>
#include <sidechain/hooks.h>
#include <signet.h>
#include <tinyformat.h>
#include <txdb.h>
#include <txmempool.h>
#include <uint256.h>
#include <undo.h>
#include <util/byte_units.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/hasher.h>
#include <util/log.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/threadpool.h>
#include <util/time.h>
#include <util/trace.h>
#include <util/translation.h>
#include <validationinterface.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>

using kernel::CCoinsStats;
using kernel::ChainstateRole;
using kernel::CoinStatsHashType;
using kernel::ComputeUTXOStats;
using kernel::Notifications;

using fsbridge::FopenFn;
using node::BlockManager;
using node::BlockMap;
using node::CBlockIndexHeightOnlyComparator;
using node::CBlockIndexWorkComparator;
using node::SnapshotMetadata;

/** Time window to wait between writing blocks/block index and chainstate to disk.
 *  Randomize writing time inside the window to prevent a situation where the
 *  network over time settles into a few cohorts of synchronized writers.
*/
static constexpr auto DATABASE_WRITE_INTERVAL_MIN{50min};
//! Changes of the sidechain state held in memory before they are written out with the coins.
static constexpr size_t SIDE_CACHE_MAX_BYTES{64 << 20};
static constexpr auto DATABASE_WRITE_INTERVAL_MAX{70min};
/** Maximum age of our tip for us to be considered current for fee estimation */
static constexpr std::chrono::hours MAX_FEE_ESTIMATION_TIP_AGE{3};
const std::vector<std::string> CHECKLEVEL_DOC {
    "level 0 reads the blocks from disk",
    "level 1 verifies block validity",
    "level 2 verifies undo data",
    "level 3 checks disconnection of tip blocks",
    "level 4 tries to reconnect the blocks",
    "each level includes the checks of the previous levels",
};
/** The number of blocks to keep below the deepest prune lock.
 *  There is nothing special about this number. It is higher than what we
 *  expect to see in regular mainnet reorgs, but not so high that it would
 *  noticeably interfere with the pruning mechanism.
 * */
static constexpr int PRUNE_LOCK_BUFFER{10};

// Return whether the completed full flush should compact chainstate
static bool ShouldCompactChainstate(bool in_ibd)
{
    static constexpr uint32_t flush_ratio{320}; // Roughly every 2 weeks with hourly flushes
    return !in_ibd && FastRandomContext().randrange(flush_ratio) == 0;
}

TRACEPOINT_SEMAPHORE(validation, block_connected);
TRACEPOINT_SEMAPHORE(utxocache, flush);
TRACEPOINT_SEMAPHORE(mempool, replaced);
TRACEPOINT_SEMAPHORE(mempool, rejected);

const CBlockIndex* Chainstate::FindForkInGlobalIndex(const CBlockLocator& locator) const
{
    AssertLockHeld(cs_main);

    // Find the latest block common to locator and chain - we expect that
    // locator.vHave is sorted descending by height.
    for (const uint256& hash : locator.vHave) {
        const CBlockIndex* pindex{m_blockman.LookupBlockIndex(hash)};
        if (pindex) {
            if (m_chain.Contains(*pindex)) {
                return pindex;
            }
            if (pindex->GetAncestor(m_chain.Height()) == m_chain.Tip()) {
                return m_chain.Tip();
            }
        }
    }
    return m_chain.Genesis();
}

bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, script_verify_flags flags, bool cacheSigStore,
                       bool cacheFullScriptStore, PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks = nullptr)
                       EXCLUSIVE_LOCKS_REQUIRED(cs_main);

bool CheckFinalTxAtTip(const CBlockIndex& active_chain_tip, const CTransaction& tx)
{
    AssertLockHeld(cs_main);

    // CheckFinalTxAtTip() uses active_chain_tip.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than active_chain_tip.Height().
    const int nBlockHeight = active_chain_tip.nHeight + 1;

    // BIP113 requires that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx().
    const int64_t nBlockTime{active_chain_tip.GetMedianTimePast()};

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

namespace {
/**
 * A helper which calculates heights of inputs of a given transaction.
 *
 * @param[in] tip    The current chain tip. If an input belongs to a mempool
 *                   transaction, we assume it will be confirmed in the next block.
 * @param[in] coins  Any CCoinsView that provides access to the relevant coins.
 * @param[in] tx     The transaction being evaluated.
 *
 * @returns A vector of input heights or nullopt, in case of an error.
 */
std::optional<std::vector<int>> CalculatePrevHeights(
    const CBlockIndex& tip,
    const CCoinsView& coins,
    const CTransaction& tx)
{
    std::vector<int> prev_heights;
    prev_heights.resize(tx.vin.size());
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        if (auto coin{coins.GetCoin(tx.vin[i].prevout)}) {
            prev_heights[i] = coin->nHeight == MEMPOOL_HEIGHT
                              ? tip.nHeight + 1 // Assume all mempool transaction confirm in the next block.
                              : coin->nHeight;
        } else {
            LogInfo("ERROR: %s: Missing input %d in transaction \'%s\'\n", __func__, i, tx.GetHash().GetHex());
            return std::nullopt;
        }
    }
    return prev_heights;
}
} // namespace

std::optional<LockPoints> CalculateLockPointsAtTip(
    CBlockIndex* tip,
    const CCoinsView& coins_view,
    const CTransaction& tx)
{
    assert(tip);

    auto prev_heights{CalculatePrevHeights(*tip, coins_view, tx)};
    if (!prev_heights.has_value()) return std::nullopt;

    CBlockIndex next_tip;
    next_tip.pprev = tip;
    // When SequenceLocks() is called within ConnectBlock(), the height
    // of the block *being* evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than active_chainstate.m_chain.Height()
    next_tip.nHeight = tip->nHeight + 1;
    const auto [min_height, min_time] = CalculateSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, prev_heights.value(), next_tip);

    // Also store the hash of the block with the highest height of
    // all the blocks which have sequence locked prevouts.
    // This hash needs to still be on the chain
    // for these LockPoint calculations to be valid
    // Note: It is impossible to correctly calculate a maxInputBlock
    // if any of the sequence locked inputs depend on unconfirmed txs,
    // except in the special case where the relative lock time/height
    // is 0, which is equivalent to no sequence lock. Since we assume
    // input height of tip+1 for mempool txs and test the resulting
    // min_height and min_time from CalculateSequenceLocks against tip+1.
    int max_input_height{0};
    for (const int height : prev_heights.value()) {
        // Can ignore mempool inputs since we'll fail if they had non-zero locks
        if (height != next_tip.nHeight) {
            max_input_height = std::max(max_input_height, height);
        }
    }

    // tip->GetAncestor(max_input_height) should never return a nullptr
    // because max_input_height is always less than the tip height.
    // It would, however, be a bad bug to continue execution, since a
    // LockPoints object with the maxInputBlock member set to nullptr
    // signifies no relative lock time.
    return LockPoints{min_height, min_time, Assert(tip->GetAncestor(max_input_height))};
}

bool CheckSequenceLocksAtTip(CBlockIndex* tip,
                             const LockPoints& lock_points)
{
    assert(tip != nullptr);

    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocksAtTip() uses active_chainstate.m_chain.Height()+1 to evaluate
    // height based locks because when SequenceLocks() is called within
    // ConnectBlock(), the height of the block *being*
    // evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than active_chainstate.m_chain.Height()
    index.nHeight = tip->nHeight + 1;

    return EvaluateSequenceLocks(index, {lock_points.height, lock_points.time});
}

static void LimitMempoolSize(CTxMemPool& pool, CCoinsViewCache& coins_cache)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, pool.cs)
{
    AssertLockHeld(::cs_main);
    AssertLockHeld(pool.cs);
    int expired = pool.Expire(GetTime<std::chrono::seconds>() - pool.m_opts.expiry);
    if (expired != 0) {
        LogDebug(BCLog::MEMPOOL, "Expired %i transactions from the memory pool\n", expired);
    }

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(pool.m_opts.max_size_bytes, &vNoSpendsRemaining);
    for (const COutPoint& removed : vNoSpendsRemaining)
        coins_cache.Uncache(removed);
}

static bool IsCurrentForFeeEstimation(Chainstate& active_chainstate) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    AssertLockHeld(cs_main);
    if (active_chainstate.m_chainman.IsInitialBlockDownload()) {
        return false;
    }
    if (active_chainstate.m_chain.Tip()->GetBlockTime() < count_seconds(GetTime<std::chrono::seconds>() - MAX_FEE_ESTIMATION_TIP_AGE))
        return false;
    if (active_chainstate.m_chain.Height() < active_chainstate.m_chainman.m_best_header->nHeight - 1) {
        return false;
    }
    return true;
}

void Chainstate::MaybeUpdateMempoolForReorg(
    DisconnectedBlockTransactions& disconnectpool,
    bool fAddToMempool)
{
    if (!m_mempool) return;

    AssertLockHeld(cs_main);
    AssertLockHeld(m_mempool->cs);
    std::vector<Txid> vHashUpdate;
    {
        // disconnectpool is ordered so that the front is the most recently-confirmed
        // transaction (the last tx of the block at the tip) in the disconnected chain.
        // Iterate disconnectpool in reverse, so that we add transactions
        // back to the mempool starting with the earliest transaction that had
        // been previously seen in a block.
        const auto queuedTx = disconnectpool.take();
        auto it = queuedTx.rbegin();
        while (it != queuedTx.rend()) {
            // ignore validation errors in resurrected transactions
            if (!fAddToMempool || (*it)->IsCoinBase() ||
                AcceptToMemoryPool(*this, *it, GetTime(),
                    /*bypass_limits=*/true, /*test_accept=*/false).m_result_type !=
                        MempoolAcceptResult::ResultType::VALID) {
                // If the transaction doesn't make it in to the mempool, remove any
                // transactions that depend on it (which would now be orphans).
                m_mempool->removeRecursive(**it, MemPoolRemovalReason::REORG);
            } else if (m_mempool->exists((*it)->GetHash())) {
                vHashUpdate.push_back((*it)->GetHash());
            }
            ++it;
        }
    }

    // AcceptToMemoryPool/addNewTransaction all assume that new mempool entries have
    // no in-mempool children, which is generally not true when adding
    // previously-confirmed transactions back to the mempool.
    // UpdateTransactionsFromBlock finds descendants of any transactions in
    // the disconnectpool that were added back and cleans up the mempool state.
    m_mempool->UpdateTransactionsFromBlock(vHashUpdate);

    // Predicate to use for filtering transactions in removeForReorg.
    // Checks whether the transaction is still final and, if it spends a coinbase output, mature.
    // Also updates valid entries' cached LockPoints if needed.
    // If false, the tx is still valid and its lockpoints are updated.
    // If true, the tx would be invalid in the next block; remove this entry and all of its descendants.
    // Note that TRUC rules are not applied here, so reorgs may cause violations of TRUC inheritance or
    // topology restrictions.
    const auto filter_final_and_mature = [&](CTxMemPool::txiter it)
        EXCLUSIVE_LOCKS_REQUIRED(m_mempool->cs, ::cs_main) {
        AssertLockHeld(m_mempool->cs);
        AssertLockHeld(::cs_main);
        const CTransaction& tx = it->GetTx();

        // The transaction must be final.
        if (!CheckFinalTxAtTip(*Assert(m_chain.Tip()), tx)) return true;

        const LockPoints& lp = it->GetLockPoints();
        // CheckSequenceLocksAtTip checks if the transaction will be final in the next block to be
        // created on top of the new chain.
        if (TestLockPointValidity(m_chain, lp)) {
            if (!CheckSequenceLocksAtTip(m_chain.Tip(), lp)) {
                return true;
            }
        } else {
            const CCoinsViewMemPool view_mempool{&CoinsTip(), *m_mempool};
            const std::optional<LockPoints> new_lock_points{CalculateLockPointsAtTip(m_chain.Tip(), view_mempool, tx)};
            if (new_lock_points.has_value() && CheckSequenceLocksAtTip(m_chain.Tip(), *new_lock_points)) {
                // Now update the mempool entry lockpoints as well.
                it->UpdateLockPoints(*new_lock_points);
            } else {
                return true;
            }
        }

        // If the transaction spends any coinbase outputs, it must be mature.
        if (it->GetSpendsCoinbase()) {
            for (const CTxIn& txin : tx.vin) {
                if (m_mempool->exists(txin.prevout.hash)) continue;
                const Coin& coin{CoinsTip().AccessCoin(txin.prevout)};
                assert(!coin.IsSpent());
                const auto mempool_spend_height{m_chain.Tip()->nHeight + 1};
                if (coin.IsCoinBase() && mempool_spend_height - coin.nHeight < m_chainman.GetConsensus().coinbase_maturity) {
                    return true;
                }
            }
        }
        // Transaction is still valid and cached LockPoints are updated.
        return false;
    };

    // We also need to remove any now-immature transactions
    m_mempool->removeForReorg(m_chain, filter_final_and_mature);
    // And the ones the drivechain rules no longer allow on top of the new tip, the reorg done.
    RemoveStaleDrivechainTxs(/*after_reorg=*/true);
    // Re-limit mempool size, in case we added any transactions
    LimitMempoolSize(*m_mempool, this->CoinsTip());
}

/**
* Checks to avoid mempool polluting consensus critical paths since cached
* signature and script validity results will be reused if we validate this
* transaction again during block validation.
* */
static bool CheckInputsFromMempoolAndCache(const CTransaction& tx, TxValidationState& state,
                const CCoinsViewCache& view, const CTxMemPool& pool,
                script_verify_flags flags, PrecomputedTransactionData& txdata, CCoinsViewCache& coins_tip,
                ValidationCache& validation_cache)
                EXCLUSIVE_LOCKS_REQUIRED(cs_main, pool.cs)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(pool.cs);

    assert(!tx.IsCoinBase());
    for (const CTxIn& txin : tx.vin) {
        const Coin& coin = view.AccessCoin(txin.prevout);

        // This coin was checked in PreChecks and MemPoolAccept
        // has been holding cs_main since then.
        Assume(!coin.IsSpent());
        if (coin.IsSpent()) return false;

        // If the Coin is available, there are 2 possibilities:
        // it is available in our current ChainstateActive UTXO set,
        // or it's a UTXO provided by a transaction in our mempool.
        // Ensure the scriptPubKeys in Coins from CoinsView are correct.
        const CTransactionRef& txFrom = pool.get(txin.prevout.hash);
        if (txFrom) {
            assert(txFrom->GetHash() == txin.prevout.hash);
            assert(txFrom->vout.size() > txin.prevout.n);
            assert(txFrom->vout[txin.prevout.n] == coin.out);
        } else {
            const Coin& coinFromUTXOSet = coins_tip.AccessCoin(txin.prevout);
            assert(!coinFromUTXOSet.IsSpent());
            assert(coinFromUTXOSet.out == coin.out);
        }
    }

    // Call CheckInputScripts() to cache signature and script validity against current tip consensus rules.
    return CheckInputScripts(tx, state, view, flags, /* cacheSigStore= */ true, /* cacheFullScriptStore= */ true, txdata, validation_cache);
}

namespace {

class MemPoolAccept
{
public:
    explicit MemPoolAccept(CTxMemPool& mempool, Chainstate& active_chainstate) :
        m_pool(mempool),
        m_view(&CoinsViewEmpty::Get()),
        m_viewmempool(&active_chainstate.CoinsTip(), m_pool),
        m_active_chainstate(active_chainstate)
    {
    }

    // We put the arguments we're handed into a struct, so we can pass them
    // around easier.
    struct ATMPArgs {
        const int64_t m_accept_time;
        const bool m_bypass_limits;
        /*
         * Return any outpoints which were not previously present in the coins
         * cache, but were added as a result of validating the tx for mempool
         * acceptance. This allows the caller to optionally remove the cache
         * additions if the associated transaction ends up being rejected by
         * the mempool.
         */
        std::vector<COutPoint>& m_coins_to_uncache;
        /** When true, the transaction or package will not be submitted to the mempool. */
        const bool m_test_accept;
        /** Whether we allow transactions to replace mempool transactions. If false,
         * any transaction spending the same inputs as a transaction in the mempool is considered
         * a conflict. */
        const bool m_allow_replacement;
        /** When true, allow sibling eviction. This only occurs in single transaction package settings. */
        const bool m_allow_sibling_eviction;
        /** Used to skip the LimitMempoolSize() call within AcceptSingleTransaction(). This should be used when multiple
         * AcceptSubPackage calls are expected and the mempool will be trimmed at the end of AcceptPackage(). */
        const bool m_package_submission;
        /** When true, use package feerates instead of individual transaction feerates for fee-based
         * policies such as mempool min fee and min relay fee.
         */
        const bool m_package_feerates;
        /** Used for local submission of transactions to catch "absurd" fees
         * due to fee miscalculation by wallets. std:nullopt implies unset, allowing any feerates.
         * Any individual transaction failing this check causes immediate failure.
         */
        const std::optional<CFeeRate> m_client_maxfeerate;

        /** Parameters for single transaction mempool validation. */
        static ATMPArgs SingleAccept(int64_t accept_time,
                                     bool bypass_limits, std::vector<COutPoint>& coins_to_uncache,
                                     bool test_accept) {
            return ATMPArgs{/*accept_time=*/ accept_time,
                            /*bypass_limits=*/ bypass_limits,
                            /*coins_to_uncache=*/ coins_to_uncache,
                            /*test_accept=*/ test_accept,
                            /*allow_replacement=*/ true,
                            /*allow_sibling_eviction=*/ true,
                            /*package_submission=*/ false,
                            /*package_feerates=*/ false,
                            /*client_maxfeerate=*/ {}, // checked by caller
            };
        }

        /** Parameters for test package mempool validation through testmempoolaccept. */
        static ATMPArgs PackageTestAccept(int64_t accept_time,
                                          std::vector<COutPoint>& coins_to_uncache) {
            return ATMPArgs{/*accept_time=*/ accept_time,
                            /*bypass_limits=*/ false,
                            /*coins_to_uncache=*/ coins_to_uncache,
                            /*test_accept=*/ true,
                            /*allow_replacement=*/ false,
                            /*allow_sibling_eviction=*/ false,
                            /*package_submission=*/ false, // not submitting to mempool
                            /*package_feerates=*/ false,
                            /*client_maxfeerate=*/ {}, // checked by caller
            };
        }

        /** Parameters for child-with-parents package validation. */
        static ATMPArgs PackageChildWithParents(int64_t accept_time,
                                                std::vector<COutPoint>& coins_to_uncache, const std::optional<CFeeRate>& client_maxfeerate) {
            return ATMPArgs{/*accept_time=*/ accept_time,
                            /*bypass_limits=*/ false,
                            /*coins_to_uncache=*/ coins_to_uncache,
                            /*test_accept=*/ false,
                            /*allow_replacement=*/ true,
                            /*allow_sibling_eviction=*/ false,
                            /*package_submission=*/ true,
                            /*package_feerates=*/ true,
                            /*client_maxfeerate=*/ client_maxfeerate,
            };
        }

        /** Parameters for a single transaction within a package. */
        static ATMPArgs SingleInPackageAccept(const ATMPArgs& package_args) {
            return ATMPArgs{/*accept_time=*/ package_args.m_accept_time,
                            /*bypass_limits=*/ false,
                            /*coins_to_uncache=*/ package_args.m_coins_to_uncache,
                            /*test_accept=*/ package_args.m_test_accept,
                            /*allow_replacement=*/ true,
                            /*allow_sibling_eviction=*/ true,
                            /*package_submission=*/ true, // trim at the end of AcceptPackage()
                            /*package_feerates=*/ false, // only 1 transaction
                            /*client_maxfeerate=*/ package_args.m_client_maxfeerate,
            };
        }

    private:
        // Private ctor to avoid exposing details to clients and allowing the possibility of
        // mixing up the order of the arguments. Use static functions above instead.
        ATMPArgs(int64_t accept_time,
                 bool bypass_limits,
                 std::vector<COutPoint>& coins_to_uncache,
                 bool test_accept,
                 bool allow_replacement,
                 bool allow_sibling_eviction,
                 bool package_submission,
                 bool package_feerates,
                 std::optional<CFeeRate> client_maxfeerate)
            : m_accept_time{accept_time},
              m_bypass_limits{bypass_limits},
              m_coins_to_uncache{coins_to_uncache},
              m_test_accept{test_accept},
              m_allow_replacement{allow_replacement},
              m_allow_sibling_eviction{allow_sibling_eviction},
              m_package_submission{package_submission},
              m_package_feerates{package_feerates},
              m_client_maxfeerate{client_maxfeerate}
        {
            // If we are using package feerates, we must be doing package submission.
            // It also means sibling eviction is not permitted.
            if (m_package_feerates) {
                Assume(m_package_submission);
                Assume(!m_allow_sibling_eviction);
            }
            if (m_allow_sibling_eviction) Assume(m_allow_replacement);
        }
    };

    /** Clean up all non-chainstate coins from m_view and m_viewmempool. */
    void CleanupTemporaryCoins() EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Single transaction acceptance
    MempoolAcceptResult AcceptSingleTransactionAndCleanup(const CTransactionRef& ptx, ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
        LOCK(m_pool.cs);
        MempoolAcceptResult result = AcceptSingleTransactionInternal(ptx, args);
        ClearSubPackageState();
        return result;
    }
    MempoolAcceptResult AcceptSingleTransactionInternal(const CTransactionRef& ptx, ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    /**
    * Multiple transaction acceptance. Transactions may or may not be interdependent, but must not
    * conflict with each other, and the transactions cannot already be in the mempool. Parents must
    * come before children if any dependencies exist.
    */
    PackageMempoolAcceptResult AcceptMultipleTransactionsAndCleanup(const std::vector<CTransactionRef>& txns, ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
        LOCK(m_pool.cs);
        PackageMempoolAcceptResult result = AcceptMultipleTransactionsInternal(txns, args);
        ClearSubPackageState();
        return result;
    }
    PackageMempoolAcceptResult AcceptMultipleTransactionsInternal(const std::vector<CTransactionRef>& txns, ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    /**
     * Submission of a subpackage.
     * If subpackage size == 1, calls AcceptSingleTransaction() with adjusted ATMPArgs to
     * enable sibling eviction and creates a PackageMempoolAcceptResult
     * wrapping the result.
     *
     * If subpackage size > 1, calls AcceptMultipleTransactions() with the provided ATMPArgs.
     *
     * Also cleans up all non-chainstate coins from m_view at the end.
    */
    PackageMempoolAcceptResult AcceptSubPackage(const std::vector<CTransactionRef>& subpackage, ATMPArgs& args)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    /**
     * Package (more specific than just multiple transactions) acceptance. Package must be a child
     * with all of its unconfirmed parents, and topologically sorted.
     */
    PackageMempoolAcceptResult AcceptPackage(const Package& package, ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

private:
    // All the intermediate state that gets passed between the various levels
    // of checking a given transaction.
    struct Workspace {
        explicit Workspace(const CTransactionRef& ptx) : m_ptx(ptx), m_hash(ptx->GetHash()) {}
        /** Txids of mempool transactions that this transaction directly conflicts with or may
         * replace via sibling eviction. */
        std::set<Txid> m_conflicts;
        /** Iterators to mempool entries that this transaction directly conflicts with or may
         * replace via sibling eviction. */
        CTxMemPool::setEntries m_iters_conflicting;
        /** All mempool parents of this transaction. */
        std::vector<CTxMemPoolEntry::CTxMemPoolEntryRef> m_parents;
        /* Handle to the tx in the changeset */
        CTxMemPool::ChangeSet::TxHandle m_tx_handle;
        /** Whether RBF-related data structures (m_conflicts, m_iters_conflicting,
         * m_replaced_transactions) include a sibling in addition to txns with conflicting inputs. */
        bool m_sibling_eviction{false};

        /** Virtual size of the transaction as used by the mempool, calculated using serialized size
         * of the transaction and sigops. */
        int64_t m_vsize;
        /** Fees paid by this transaction: total input amounts subtracted by total output amounts. */
        CAmount m_base_fees;
        /** Base fees + any fee delta set by the user with prioritisetransaction. */
        CAmount m_modified_fees;

        /** If we're doing package validation (i.e. m_package_feerates=true), the "effective"
         * package feerate of this transaction is the total fees divided by the total size of
         * transactions (which may include its ancestors and/or descendants). */
        CFeeRate m_package_feerate{0};

        const CTransactionRef& m_ptx;
        /** Txid. */
        const Txid& m_hash;
        TxValidationState m_state;
        /** A temporary cache containing serialized transaction data for signature verification.
         * Reused across PolicyScriptChecks and ConsensusScriptChecks. */
        PrecomputedTransactionData m_precomputed_txdata;
    };

    // Check a transaction against the drivechain rules for sidechain escrow outputs.
    bool DrivechainChecks(Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Run the policy checks on a given transaction, excluding any script checks.
    // Looks up inputs, calculates feerate, considers replacement, evaluates
    // package limits, etc. As this function can be invoked for "free" by a peer,
    // only tests that are fast should be done here (to avoid CPU DoS).
    bool PreChecks(ATMPArgs& args, Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Run checks for mempool replace-by-fee, only used in AcceptSingleTransaction.
    bool ReplacementChecks(Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    bool PackageRBFChecks(const std::vector<CTransactionRef>& txns,
                          std::vector<Workspace>& workspaces,
                          PackageValidationState& package_state) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Run the script checks using our policy flags. As this can be slow, we should
    // only invoke this on transactions that have otherwise passed policy checks.
    bool PolicyScriptChecks(Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Re-run the script checks, using consensus flags, and try to cache the
    // result in the scriptcache. This should be done after
    // PolicyScriptChecks(). This requires that all inputs either be in our
    // utxo set or in the mempool.
    bool ConsensusScriptChecks(Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Try to add the transaction to the mempool, removing any conflicts first.
    void FinalizeSubpackage(const ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Submit all transactions to the mempool and call ConsensusScriptChecks to add to the script
    // cache - should only be called after successful validation of all transactions in the package.
    // Does not call LimitMempoolSize(), so mempool max_size_bytes may be temporarily exceeded.
    bool SubmitPackage(const ATMPArgs& args, std::vector<Workspace>& workspaces, PackageValidationState& package_state,
                       std::map<Wtxid, MempoolAcceptResult>& results)
         EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Compare a package's feerate against minimum allowed.
    bool CheckFeeRate(size_t package_size, CAmount package_fee, TxValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_pool.cs)
    {
        AssertLockHeld(::cs_main);
        AssertLockHeld(m_pool.cs);
        CAmount mempoolRejectFee = m_pool.GetMinFee().GetFee(package_size);
        if (mempoolRejectFee > 0 && package_fee < mempoolRejectFee) {
            return state.Invalid(TxValidationResult::TX_RECONSIDERABLE, "mempool min fee not met", strprintf("%d < %d", package_fee, mempoolRejectFee));
        }

        if (package_fee < m_pool.m_opts.min_relay_feerate.GetFee(package_size)) {
            return state.Invalid(TxValidationResult::TX_RECONSIDERABLE, "min relay fee not met",
                                 strprintf("%d < %d", package_fee, m_pool.m_opts.min_relay_feerate.GetFee(package_size)));
        }
        return true;
    }

    ValidationCache& GetValidationCache()
    {
        return m_active_chainstate.m_chainman.m_validation_cache;
    }

private:
    CTxMemPool& m_pool;

    /** Holds a cached view of available coins from the UTXO set, mempool, and artificial temporary coins (to enable package validation).
     * The view doesn't track whether a coin previously existed but has now been spent. We detect conflicts in other ways:
     * - conflicts within a transaction are checked in CheckTransaction (bad-txns-inputs-duplicate)
     * - conflicts within a package are checked in IsWellFormedPackage (conflict-in-package)
     * - conflicts with an existing mempool transaction are found in CTxMemPool::GetConflictTx and replacements are allowed
     * The temporary coins should persist between individual transaction checks so that package validation is possible,
     * but must be cleaned up when we finish validating a subpackage, whether accepted or rejected. The cache must also
     * be cleared when mempool contents change (when a changeset is applied or when the mempool trims itself) because it
     * can return cached coins that no longer exist in the backend. Use CleanupTemporaryCoins() anytime you are finished
     * with a SubPackageState or call LimitMempoolSize().
     */
    CCoinsViewCache m_view;

    // These are the two possible backends for m_view.
    /** When m_view is connected to m_viewmempool as its backend, it can pull coins from the mempool and from the UTXO
     * set. This is also where temporary coins are stored. */
    CCoinsViewMemPool m_viewmempool;

    Chainstate& m_active_chainstate;

    // Fields below are per *sub*package state and must be reset prior to subsequent
    // AcceptSingleTransaction and AcceptMultipleTransactions invocations
    struct SubPackageState {
        /** Aggregated modified fees of all transactions, used to calculate package feerate. */
        CAmount m_total_modified_fees{0};
        /** Aggregated virtual size of all transactions, used to calculate package feerate. */
        int64_t m_total_vsize{0};

        // RBF-related members
        /** Whether the transaction(s) would replace any mempool transactions and/or evict any siblings.
         * If so, RBF rules apply. */
        bool m_rbf{false};
        /** Mempool transactions that were replaced. */
        std::list<CTransactionRef> m_replaced_transactions;
        /* Changeset representing adding transactions and removing their conflicts. */
        std::unique_ptr<CTxMemPool::ChangeSet> m_changeset;

        /** Total modified fees of mempool transactions being replaced. */
        CAmount m_conflicting_fees{0};
        /** Total size (in virtual bytes) of mempool transactions being replaced. */
        size_t m_conflicting_size{0};
    };

    struct SubPackageState m_subpackage;

    /** Re-set sub-package state to not leak between evaluations */
    void ClearSubPackageState() EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs)
    {
        m_subpackage = SubPackageState{};

        // And clean coins while at it
        CleanupTemporaryCoins();
    }
};

bool MemPoolAccept::DrivechainChecks(Workspace& ws)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);
    const CTransaction& tx{*ws.m_ptx};
    const Consensus::DrivechainParams& params{m_active_chainstate.m_chainman.GetConsensus().drivechain};

    // On a sidechain: withdrawals have to be well formed, and a refund request has to be one that the next block can honour.
    if (const Consensus::SidechainParams& side_params{m_active_chainstate.m_chainman.GetConsensus().sidechain}; side_params.enabled) {
        const sidechain::State side{m_active_chainstate.SideState()};
        for (const CTxOut& out : tx.vout) {
            if (sidechain::IsWithdrawalScript(out.scriptPubKey)) {
                const auto withdrawal{sidechain::ParseWithdrawalOutput(out)};
                if (!withdrawal) return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "bad-sc-withdrawal");
                if (withdrawal->amount < side_params.min_withdrawal) return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "bad-sc-withdrawal-amount");
            } else if (const auto refund{sidechain::ParseRefundScript(out.scriptPubKey)}) {
                std::string reject_reason;
                const bool main_pending{side.MainPendingNext(*Assert(m_active_chainstate.m_chainman.m_mainchain), side_params)};
                if (!side.CheckRefund(*refund, reject_reason, main_pending)) return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, reject_reason);
                if (const auto other{m_pool.m_refunds.find(refund->withdrawal)}; other != m_pool.m_refunds.end() && !ws.m_conflicts.contains(other->second)) {
                    return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "sc-refund-in-mempool");
                }
            }
        }

        // The shielded pool: the bundle against the pool as the chain leaves it, and against the
        // other transactions of the mempool, which may not reveal the same nullifiers (those that do
        // are conflicts, PreChecks: replaced, or this one is refused). The proof is checked last
        // (PolicyScriptChecks), once the transaction pays its way.
        std::string reject_reason;
        std::string error;
        if (const auto bundle{zside::Bundle::FromTx(tx, error)}) {
            for (const uint256& nf : bundle->Nullifiers()) {
                if (const auto other{m_pool.m_shielded_nullifiers.find(nf)}; other != m_pool.m_shielded_nullifiers.end() && !ws.m_conflicts.contains(other->second)) {
                    return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "zs-nullifier-in-mempool");
                }
            }
        }
        if (!side.Shielded().CheckState(tx, reject_reason)) {
            return ws.m_state.Invalid(TxValidationResult::TX_CONSENSUS, reject_reason);
        }
        // The rules of the sidechain's own, as the next block would apply them (none in the template),
        // on an overlay that is dropped.
        sidechain::StoreOverlay scratch{side.View(), /*journal=*/false};
        sidechain::State scratch_state{scratch};
        std::vector<CTxOut> payouts;
        const int next_height{m_active_chainstate.m_chain.Height() + 1};
        if (!sidechain::Hooks().ApplyTx(scratch_state, tx, next_height, /*peg_step=*/false, payouts, reject_reason)) {
            return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, reject_reason);
        }
    }

    // The sidechains whose escrow this transaction touches.
    std::set<drivechain::SidechainId> slots;
    for (const CTxOut& out : tx.vout) {
        if (const auto slot{drivechain::ParseEscrowScript(out.scriptPubKey)}; slot && *slot < params.max_sidechains) slots.insert(*slot);
    }
    for (const CTxIn& in : tx.vin) {
        const auto slot{drivechain::ParseEscrowScript(m_view.AccessCoin(in.prevout).out.scriptPubKey)};
        if (!slot || *slot >= params.max_sidechains) continue;
        slots.insert(*slot);
        // A treasury output is spent with nothing: anything more would only let a third party pad a
        // deposit or a withdrawal, whose fee is fixed, so that the original cannot replace it.
        if (!in.scriptSig.empty() || !in.scriptWitness.IsNull()) {
            return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-escrow-input-not-empty");
        }
    }
    if (slots.empty()) return true;

    // The escrow of those sidechains as the mempool leaves it, up to the transactions this one would replace.
    // m_pool is the active chainstate's mempool: lock it as the chainstate names it (recursively).
    LOCK(m_active_chainstate.MempoolMutex());
    drivechain::SidechainDB scdb{m_active_chainstate.GetMempoolSidechainDB(slots, &ws.m_conflicts)};
    drivechain::SidechainDB::EscrowOutputs escrow_outputs{scdb.GetEscrowOutputs()};
    drivechain::BlockUndo undo;
    std::string reject_reason;
    // A withdrawal is welcome once its bundle has the work score (BIP300 M6): any miner can then mine it.
    if (!scdb.ConnectTx(tx, params, escrow_outputs, undo, nullptr, reject_reason)) {
        return ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, reject_reason);
    }
    return true;
}

bool MemPoolAccept::PreChecks(ATMPArgs& args, Workspace& ws)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);
    const CTransactionRef& ptx = ws.m_ptx;
    const CTransaction& tx = *ws.m_ptx;
    const Txid& hash = ws.m_hash;

    // Copy/alias what we need out of args
    const int64_t nAcceptTime = args.m_accept_time;
    const bool bypass_limits = args.m_bypass_limits;
    std::vector<COutPoint>& coins_to_uncache = args.m_coins_to_uncache;

    // Alias what we need out of ws
    TxValidationState& state = ws.m_state;

    if (!CheckTransaction(tx, state)) {
        return false; // state filled in by CheckTransaction
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (m_pool.m_opts.require_standard && !IsStandardTx(tx, m_pool.m_opts.max_datacarrier_bytes, m_pool.m_opts.permit_bare_multisig, m_pool.m_opts.dust_relay_feerate, reason)) {
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, reason);
    }

    // Transactions smaller than 65 non-witness bytes are not relayed to mitigate CVE-2017-12842.
    if (::GetSerializeSize(TX_NO_WITNESS(tx)) < MIN_STANDARD_TX_NONWITNESS_SIZE)
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, "tx-size-small");

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTxAtTip(*Assert(m_active_chainstate.m_chain.Tip()), tx)) {
        return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "non-final");
    }

    if (m_pool.exists(tx.GetWitnessHash())) {
        // Exact transaction already exists in the mempool.
        return state.Invalid(TxValidationResult::TX_CONFLICT, "txn-already-in-mempool");
    } else if (m_pool.exists(tx.GetHash())) {
        // Transaction with the same non-witness data but different witness (same txid, different
        // wtxid) already exists in the mempool.
        return state.Invalid(TxValidationResult::TX_CONFLICT, "txn-same-nonwitness-data-in-mempool");
    }

    // Check for conflicts with in-memory transactions
    for (const CTxIn &txin : tx.vin)
    {
        const CTransaction* ptxConflicting = m_pool.GetConflictTx(txin.prevout);
        if (ptxConflicting) {
            if (!args.m_allow_replacement) {
                // Transaction conflicts with a mempool tx, but we're not allowing replacements in this context.
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "bip125-replacement-disallowed");
            }
            ws.m_conflicts.insert(ptxConflicting->GetHash());
        }
    }

    // Requests for blind merged mining: a block can serve one per sidechain,
    // and only if it is built on the block the request names. A request for a
    // sidechain that already has one in the mempool competes with it under the
    // replacement rules.
    {
        const Consensus::DrivechainParams& dc_params{m_active_chainstate.m_chainman.GetConsensus().drivechain};
        const uint256 tip_hash{m_active_chainstate.m_chain.Tip()->GetBlockHash()};
        std::set<drivechain::SidechainId> requested;
        for (const drivechain::BmmRequest& request : drivechain::GetBmmRequests(tx)) {
            if (request.slot >= dc_params.max_sidechains) continue;
            if (!m_active_chainstate.m_scdb.IsActive(request.slot)) {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-bmm-inactive-sidechain");
            }
            if (request.prev_main_block_hash != tip_hash) {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-bmm-prev-block");
            }
            if (!requested.insert(request.slot).second) {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-bmm-multiple-requests");
            }
            if (const auto other{m_pool.m_bmm_requests.find(request.slot)}; other != m_pool.m_bmm_requests.end() && other->second != hash) {
                if (!args.m_allow_replacement) {
                    return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "bip125-replacement-disallowed");
                }
                ws.m_conflicts.insert(other->second);
            }
        }
    }

    // zSide: a shielded transaction that reveals a nullifier one of the mempool reveals spends the same
    // note: a conflict, as one that spends the same coin, which it replaces under the same rules.
    if (m_active_chainstate.m_chainman.GetConsensus().sidechain.enabled && zside::HasShieldedMarker(tx)) {
        std::string error;
        if (const auto bundle{zside::Bundle::FromTx(tx, error)}) {
            for (const uint256& nf : bundle->Nullifiers()) {
                const auto other{m_pool.m_shielded_nullifiers.find(nf)};
                if (other == m_pool.m_shielded_nullifiers.end() || other->second == hash) continue;
                if (!args.m_allow_replacement) {
                    return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "bip125-replacement-disallowed");
                }
                ws.m_conflicts.insert(other->second);
            }
        }
    }

    m_view.SetBackend(m_viewmempool);

    const CCoinsViewCache& coins_cache = m_active_chainstate.CoinsTip();
    // do all inputs exist?
    for (const CTxIn& txin : tx.vin) {
        if (!coins_cache.HaveCoinInCache(txin.prevout)) {
            coins_to_uncache.push_back(txin.prevout);
        }

        // Note: this call may add txin.prevout to the coins cache
        // (coins_cache.cacheCoins) by way of FetchCoin(). It should be removed
        // later (via coins_to_uncache) if this tx turns out to be invalid.
        if (!m_view.HaveCoin(txin.prevout)) {
            // Are inputs missing because we already have the tx?
            for (size_t out = 0; out < tx.vout.size(); out++) {
                // Optimistically just do efficient check of cache for outputs
                if (coins_cache.HaveCoinInCache(COutPoint(hash, out))) {
                    return state.Invalid(TxValidationResult::TX_CONFLICT, "txn-already-known");
                }
            }
            // Otherwise assume this might be an orphan tx for which we just haven't seen parents yet
            return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent");
        }
    }

    // This is const, but calls into `CCoinsViewCache::GetBestBlock()` to refresh
    // the cached best block through `m_viewmempool` after caching inputs.
    (void)m_view.GetBestBlock();

    // All required inputs are cached now, so switch m_view to the empty backend.
    // This keeps already-fetched cache entries for later checks and prevents new
    // backend lookups (which would avoid coins_to_uncache tracking).
    m_view.SetBackend(CoinsViewEmpty::Get());

    assert(m_active_chainstate.m_blockman.LookupBlockIndex(m_view.GetBestBlock()) == m_active_chainstate.m_chain.Tip());

    // Only accept BIP68 sequence locked transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    // Pass in m_view which has all of the relevant inputs cached. Note that, since m_view's
    // backend was removed, it no longer pulls coins from the mempool.
    const std::optional<LockPoints> lock_points{CalculateLockPointsAtTip(m_active_chainstate.m_chain.Tip(), m_view, tx)};
    if (!lock_points.has_value() || !CheckSequenceLocksAtTip(m_active_chainstate.m_chain.Tip(), *lock_points)) {
        return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "non-BIP68-final");
    }

    // The mempool holds txs for the next block, so pass height+1 to CheckTxInputs
    if (!Consensus::CheckTxInputs(tx, state, m_view, m_active_chainstate.m_chain.Height() + 1, ws.m_base_fees, m_active_chainstate.m_chainman.GetConsensus().coinbase_maturity)) {
        return false; // state filled in by CheckTxInputs
    }

    // A transaction that creates or spends a sidechain escrow output has to be
    // a deposit or withdrawal a block could contain, given those already in the mempool.
    if (!DrivechainChecks(ws)) return false;

    // A BMM request or a treasury transaction (deposit, withdrawal) in the mempool is one others
    // compete with: the next request of its sidechain replaces it, the next treasury transaction
    // follows it. Like a TRUC transaction it takes no unconfirmed children that could pin it there,
    // with their size or their low fee, but small ones of its kind: the next treasury transaction
    // through the escrow output (a withdrawal of any size), and a request or treasury transaction paid from its change (a
    // wallet that serves several sidechains). Not for transactions a reorg brings back.
    if (!bypass_limits) {
        const uint32_t max_sidechains{m_active_chainstate.m_chainman.GetConsensus().drivechain.max_sidechains};
        const auto is_treasury{[&](const CTransaction& t) {
            return std::any_of(t.vout.begin(), t.vout.end(), [&](const CTxOut& out) {
                const auto slot{drivechain::ParseEscrowScript(out.scriptPubKey)};
                return slot && *slot < max_sidechains;
            });
        }};
        const bool bmm_request{drivechain::GetBmmRequest(tx).has_value()};
        const bool drivechain_tx{bmm_request || is_treasury(tx)};
        // Whatever its parents: a BMM request, which the next request of its sidechain has to
        // replace, and a deposit, which the next treasury transaction of its sidechain spends,
        // are kept small, so that a large one at a low fee rate cannot make replacing or
        // following it expensive. (A withdrawal, of any size, pays a bundle the miners voted through.)
        if (drivechain_tx && GetVirtualTransactionSize(tx) > MAX_DRIVECHAIN_TX_VSIZE) {
            bool withdrawal{false};
            for (const CTxIn& in : tx.vin) {
                const CTxOut& spent{m_view.AccessCoin(in.prevout).out};
                const auto slot{drivechain::ParseEscrowScript(spent.scriptPubKey)};
                if (!slot || *slot >= max_sidechains) continue;
                withdrawal = std::any_of(tx.vout.begin(), tx.vout.end(), [&](const CTxOut& out) {
                    return drivechain::ParseEscrowScript(out.scriptPubKey) == slot && out.nValue < spent.nValue;
                });
                break;
            }
            if (bmm_request || !withdrawal) {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-tx-too-large",
                                     strprintf("a %s of more than %u vbytes", bmm_request ? "BMM request" : "deposit", MAX_DRIVECHAIN_TX_VSIZE));
            }
        }
        for (const CTxIn& in : tx.vin) {
            const CTransactionRef parent{m_pool.get(in.prevout.hash)};
            if (!parent || in.prevout.n >= parent->vout.size()) continue;
            // The escrow output: spent only by the next treasury transaction, as the escrow rules have
            // it. A deposit chained on an unconfirmed one is small, like a TRUC child: a large one at
            // a low fee rate would hold up every deposit chained after it. (A withdrawal, of any size,
            // pays a bundle the miners voted through.)
            if (const auto slot{drivechain::ParseEscrowScript(parent->vout[in.prevout.n].scriptPubKey)}; slot && *slot < max_sidechains) {
                const bool withdrawal{std::any_of(tx.vout.begin(), tx.vout.end(), [&](const CTxOut& out) {
                    return drivechain::ParseEscrowScript(out.scriptPubKey) == slot && out.nValue < parent->vout[in.prevout.n].nValue;
                })};
                if (!withdrawal && GetVirtualTransactionSize(tx) > TRUC_CHILD_MAX_VSIZE) {
                    return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-unconfirmed-parent",
                                         strprintf("a deposit of more than %u vbytes spends the escrow output of %s, which is not confirmed", TRUC_CHILD_MAX_VSIZE, parent->GetHash().ToString()));
                }
                continue;
            }
            if (!drivechain::GetBmmRequest(*parent) && !is_treasury(*parent)) continue;
            if (!drivechain_tx || GetVirtualTransactionSize(tx) > TRUC_CHILD_MAX_VSIZE) {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "dc-unconfirmed-parent",
                                     strprintf("spends %s, a BMM request or treasury transaction that is not confirmed", parent->GetHash().ToString()));
            }
        }
    }

    if (m_pool.m_opts.require_standard) {
        state = ValidateInputsStandardness(tx, m_view);
        if (state.IsInvalid()) {
            return false;
        }
    }

    // Check for non-standard witnesses.
    if (tx.HasWitness() && m_pool.m_opts.require_standard && !IsWitnessStandard(tx, m_view)) {
        return state.Invalid(TxValidationResult::TX_WITNESS_MUTATED, "bad-witness-nonstandard");
    }

    int64_t nSigOpsCost = GetTransactionSigOpCost(tx, m_view, STANDARD_SCRIPT_VERIFY_FLAGS);

    // Keep track of transactions that spend a coinbase, which we re-scan
    // during reorgs to ensure COINBASE_MATURITY is still met.
    bool fSpendsCoinbase = false;
    for (const CTxIn &txin : tx.vin) {
        const Coin &coin = m_view.AccessCoin(txin.prevout);
        if (coin.IsCoinBase()) {
            fSpendsCoinbase = true;
            break;
        }
    }

    // Set entry_sequence to 0 when bypass_limits is used; this allows txs from a block
    // reorg to be marked earlier than any child txs that were already in the mempool.
    const uint64_t entry_sequence = bypass_limits ? 0 : m_pool.GetSequence();
    if (!m_subpackage.m_changeset) {
        m_subpackage.m_changeset = m_pool.GetChangeSet();
    }
    ws.m_tx_handle = m_subpackage.m_changeset->StageAddition(ptx, ws.m_base_fees, nAcceptTime, m_active_chainstate.m_chain.Height(), entry_sequence, fSpendsCoinbase, nSigOpsCost, lock_points.value());

    // ws.m_modified_fees includes any fee deltas from PrioritiseTransaction
    ws.m_modified_fees = ws.m_tx_handle->GetModifiedFee();

    ws.m_vsize = ws.m_tx_handle->GetTxSize();

    // Enforces 0-fee for dust transactions, no incentive to be mined alone
    if (m_pool.m_opts.require_standard) {
        if (!PreCheckEphemeralTx(*ptx, m_pool.m_opts.dust_relay_feerate, ws.m_base_fees, ws.m_modified_fees, state)) {
            return false; // state filled in by PreCheckEphemeralTx
        }
    }

    if (nSigOpsCost > MAX_STANDARD_TX_SIGOPS_COST)
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, "bad-txns-too-many-sigops",
                strprintf("%d", nSigOpsCost));

    // No individual transactions are allowed below the mempool min feerate except from disconnected
    // blocks and transactions in a package. Package transactions will be checked using package
    // feerate later.
    if (!bypass_limits && !args.m_package_feerates && !CheckFeeRate(ws.m_vsize, ws.m_modified_fees, state)) return false;

    ws.m_iters_conflicting = m_pool.GetIterSet(ws.m_conflicts);

    ws.m_parents = m_pool.GetParents(*ws.m_tx_handle);

    if (!args.m_bypass_limits) {
        // Perform the TRUC checks, using the in-mempool parents.
        if (const auto err{SingleTRUCChecks(m_pool, ws.m_ptx, ws.m_parents, ws.m_conflicts, ws.m_vsize)}) {
            // Single transaction contexts only.
            if (args.m_allow_sibling_eviction && err->second != nullptr) {
                // We should only be considering where replacement is considered valid as well.
                Assume(args.m_allow_replacement);
                // Potential sibling eviction. Add the sibling to our list of mempool conflicts to be
                // included in RBF checks.
                ws.m_conflicts.insert(err->second->GetHash());
                // Adding the sibling to m_iters_conflicting here means that it doesn't count towards
                // RBF Carve Out above. This is correct, since removing to-be-replaced transactions from
                // the descendant count is done separately in SingleTRUCChecks for TRUC transactions.
                ws.m_iters_conflicting.insert(m_pool.GetIter(err->second->GetHash()).value());
                ws.m_sibling_eviction = true;
                // The sibling will be treated as part of the to-be-replaced set in ReplacementChecks.
                // Note that we are not checking whether it opts in to replaceability via BIP125 or TRUC
                // (which is normally done in PreChecks). However, the only way a TRUC transaction can
                // have a non-TRUC and non-BIP125 descendant is due to a reorg.
            } else {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "TRUC-violation", err->first);
            }
        }
    }

    // We want to detect conflicts in any tx in a package to trigger package RBF logic
    m_subpackage.m_rbf |= !ws.m_conflicts.empty();
    return true;
}

bool MemPoolAccept::ReplacementChecks(Workspace& ws)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);

    const CTransaction& tx = *ws.m_ptx;
    const Txid& hash = ws.m_hash;
    TxValidationState& state = ws.m_state;

    CTxMemPool::setEntries all_conflicts;

    // Calculate all conflicting entries and enforce Rule #5.
    if (const auto err_string{GetEntriesForConflicts(tx, m_pool, ws.m_iters_conflicting, all_conflicts)}) {
        return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY,
                             strprintf("too many potential replacements%s", ws.m_sibling_eviction ? " (including sibling eviction)" : ""), *err_string);
    }

    // Check if it's economically rational to mine this transaction rather than the ones it
    // replaces and pays for its own relay fees. Enforce Rules #3 and #4.
    for (CTxMemPool::txiter it : all_conflicts) {
        m_subpackage.m_conflicting_fees += it->GetModifiedFee();
        m_subpackage.m_conflicting_size += it->GetTxSize();
    }

    if (const auto err_string{PaysForRBF(m_subpackage.m_conflicting_fees, ws.m_modified_fees, ws.m_vsize,
                                         m_pool.m_opts.incremental_relay_feerate, hash)}) {
        // Result may change in a package context
        return state.Invalid(TxValidationResult::TX_RECONSIDERABLE,
                             strprintf("insufficient fee%s", ws.m_sibling_eviction ? " (including sibling eviction)" : ""), *err_string);
    }

    // Add all the to-be-removed transactions to the changeset.
    for (auto it : all_conflicts) {
        m_subpackage.m_changeset->StageRemoval(it);
    }

    // Run cluster size limit checks and fail if we exceed them.
    if (!m_subpackage.m_changeset->CheckMemPoolPolicyLimits()) {
        return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "too-large-cluster", "");
    }

    if (const auto err_string{ImprovesFeerateDiagram(*m_subpackage.m_changeset)}) {
        // We checked above for the cluster size limits being respected, so a
        // failure here can only be due to an insufficient fee.
        Assume(err_string->first == DiagramCheckError::FAILURE);
        return state.Invalid(TxValidationResult::TX_RECONSIDERABLE, "replacement-failed", err_string->second);
    }

    return true;
}

bool MemPoolAccept::PackageRBFChecks(const std::vector<CTransactionRef>& txns,
                                     std::vector<Workspace>& workspaces,
                                     PackageValidationState& package_state)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);

    assert(std::all_of(txns.cbegin(), txns.cend(), [this](const auto& tx)
                       { return !m_pool.exists(tx->GetHash());}));

    assert(txns.size() == workspaces.size());

    // We're in package RBF context; replacement proposal must be size 2
    if (workspaces.size() != 2 || !Assume(IsChildWithParents(txns))) {
        return package_state.Invalid(PackageValidationResult::PCKG_POLICY, "package RBF failed: package must be 1-parent-1-child");
    }

    // If the package has in-mempool parents, we won't consider a package RBF
    // since it would result in a cluster larger than 2.
    // N.B. To relax this constraint we will need to revisit how CCoinsViewMemPool::PackageAddTransaction
    // is being used inside AcceptMultipleTransactions to track available inputs while processing a package.
    // Specifically we would need to check that the ancestors of the new
    // transactions don't intersect with the set of transactions to be removed
    // due to RBF, which is not checked at all in the package acceptance
    // context.
    for (const auto& ws : workspaces) {
        if (!ws.m_parents.empty()) {
            return package_state.Invalid(PackageValidationResult::PCKG_POLICY, "package RBF failed: new transaction cannot have mempool ancestors");
        }
    }

    // Aggregate all conflicts into one set.
    CTxMemPool::setEntries direct_conflict_iters;
    for (Workspace& ws : workspaces) {
        // Aggregate all conflicts into one set.
        direct_conflict_iters.merge(ws.m_iters_conflicting);
    }

    const auto& parent_ws = workspaces[0];
    const auto& child_ws = workspaces[1];

    // Don't consider replacements that would cause us to remove a large number of mempool entries.
    // This limit is not increased in a package RBF. Use the aggregate number of transactions.
    CTxMemPool::setEntries all_conflicts;
    if (const auto err_string{GetEntriesForConflicts(*child_ws.m_ptx, m_pool, direct_conflict_iters,
                                                     all_conflicts)}) {
        return package_state.Invalid(PackageValidationResult::PCKG_POLICY,
                                     "package RBF failed: too many potential replacements", *err_string);
    }

    for (CTxMemPool::txiter it : all_conflicts) {
        m_subpackage.m_changeset->StageRemoval(it);
        m_subpackage.m_conflicting_fees += it->GetModifiedFee();
        m_subpackage.m_conflicting_size += it->GetTxSize();
    }

    // Use the child as the transaction for attributing errors to.
    const Txid& child_hash = child_ws.m_ptx->GetHash();
    if (const auto err_string{PaysForRBF(/*original_fees=*/m_subpackage.m_conflicting_fees,
                                         /*replacement_fees=*/m_subpackage.m_total_modified_fees,
                                         /*replacement_vsize=*/m_subpackage.m_total_vsize,
                                         m_pool.m_opts.incremental_relay_feerate, child_hash)}) {
        return package_state.Invalid(PackageValidationResult::PCKG_POLICY,
                                     "package RBF failed: insufficient anti-DoS fees", *err_string);
    }

    // Ensure this two transaction package is a "chunk" on its own; we don't want the child
    // to be only paying anti-DoS fees
    const CFeeRate parent_feerate(parent_ws.m_modified_fees, parent_ws.m_vsize);
    const CFeeRate package_feerate(m_subpackage.m_total_modified_fees, m_subpackage.m_total_vsize);
    if (package_feerate <= parent_feerate) {
        return package_state.Invalid(PackageValidationResult::PCKG_POLICY,
                                     "package RBF failed: package feerate is less than or equal to parent feerate",
                                     strprintf("package feerate %s <= parent feerate is %s", package_feerate.ToString(), parent_feerate.ToString()));
    }

    // Run cluster size limit checks and fail if we exceed them.
    if (!m_subpackage.m_changeset->CheckMemPoolPolicyLimits()) {
        return package_state.Invalid(PackageValidationResult::PCKG_POLICY, "too-large-cluster", "");
    }

    // Check if it's economically rational to mine this package rather than the ones it replaces.
    if (const auto err_tup{ImprovesFeerateDiagram(*m_subpackage.m_changeset)}) {
        Assume(err_tup->first == DiagramCheckError::FAILURE);
        return package_state.Invalid(PackageValidationResult::PCKG_POLICY,
                                     "package RBF failed: " + err_tup.value().second, "");
    }

    LogDebug(BCLog::TXPACKAGES, "package RBF checks passed: parent %s (wtxid=%s), child %s (wtxid=%s), package hash (%s)\n",
        txns.front()->GetHash().ToString(), txns.front()->GetWitnessHash().ToString(),
        txns.back()->GetHash().ToString(), txns.back()->GetWitnessHash().ToString(),
        GetPackageHash(txns).ToString());


    return true;
}

bool MemPoolAccept::PolicyScriptChecks(Workspace& ws)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);
    const CTransaction& tx = *ws.m_ptx;
    TxValidationState& state = ws.m_state;

    constexpr script_verify_flags scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;

    // Check input scripts and signatures.
    // This is done last to help prevent CPU exhaustion denial-of-service attacks.
    if (!CheckInputScripts(tx, state, m_view, scriptVerifyFlags, true, false, ws.m_precomputed_txdata, GetValidationCache())) {
        // Detect a failure due to a missing witness so that p2p code can handle rejection caching appropriately.
        if (!tx.HasWitness() && SpendsNonAnchorWitnessProg(tx, m_view)) {
            state.Invalid(TxValidationResult::TX_WITNESS_STRIPPED,
                    state.GetRejectReason(), state.GetDebugMessage());
        }
        return false; // state filled in by CheckInputScripts
    }

    // zSide: the proof and signatures of a shielded bundle, the costliest check of all, last: once
    // the transaction pays its way and fits the pool (DrivechainChecks).
    if (m_active_chainstate.m_chainman.GetConsensus().sidechain.enabled && zside::HasShieldedMarker(tx)) {
        std::string reject_reason;
        if (!zside::CheckShieldedTx(tx, reject_reason, zside::SighashDomainOf(m_active_chainstate.m_chainman.GetConsensus()))) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, reject_reason);
        }
    }

    return true;
}

bool MemPoolAccept::ConsensusScriptChecks(Workspace& ws)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);
    const CTransaction& tx = *ws.m_ptx;
    const Txid& hash = ws.m_hash;
    TxValidationState& state = ws.m_state;

    // Check again against the current block tip's script verification
    // flags to cache our script execution flags. This is, of course,
    // useless if the next block has different script flags from the
    // previous one, but because the cache tracks script flags for us it
    // will auto-invalidate and we'll just have a few blocks of extra
    // misses on soft-fork activation.
    //
    // This is also useful in case of bugs in the standard flags that cause
    // transactions to pass as valid when they're actually invalid. For
    // instance the STRICTENC flag was incorrectly allowing certain
    // CHECKSIG NOT scripts to pass, even though they were invalid.
    //
    // There is a similar check in CreateNewBlock() to prevent creating
    // invalid blocks (using TestBlockValidity), however allowing such
    // transactions into the mempool can be exploited as a DoS attack.
    script_verify_flags currentBlockScriptVerifyFlags{GetBlockScriptFlags(*m_active_chainstate.m_chain.Tip(), m_active_chainstate.m_chainman)};
    if (!CheckInputsFromMempoolAndCache(tx, state, m_view, m_pool, currentBlockScriptVerifyFlags,
                                        ws.m_precomputed_txdata, m_active_chainstate.CoinsTip(), GetValidationCache())) {
        LogError("BUG! PLEASE REPORT THIS! CheckInputScripts failed against latest-block but not STANDARD flags %s, %s", hash.ToString(), state.ToString());
        return Assume(false);
    }

    return true;
}

void MemPoolAccept::FinalizeSubpackage(const ATMPArgs& args)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);

    if (!m_subpackage.m_changeset->GetRemovals().empty()) Assume(args.m_allow_replacement);
    // Remove conflicting transactions from the mempool
    for (CTxMemPool::txiter it : m_subpackage.m_changeset->GetRemovals())
    {
        std::string log_string = strprintf("replacing mempool tx %s (wtxid=%s, fees=%s, vsize=%s). ",
                                      it->GetTx().GetHash().ToString(),
                                      it->GetTx().GetWitnessHash().ToString(),
                                      it->GetFee(),
                                      it->GetTxSize());
        FeeFrac feerate{m_subpackage.m_total_modified_fees, int32_t(m_subpackage.m_total_vsize)};
        uint256 tx_or_package_hash{};
        const bool replaced_with_tx{m_subpackage.m_changeset->GetTxCount() == 1};
        if (replaced_with_tx) {
            const CTransaction& tx = m_subpackage.m_changeset->GetAddedTxn(0);
            tx_or_package_hash = tx.GetHash().ToUint256();
            log_string += strprintf("New tx %s (wtxid=%s, fees=%s, vsize=%s)",
                                    tx.GetHash().ToString(),
                                    tx.GetWitnessHash().ToString(),
                                    feerate.fee,
                                    feerate.size);
        } else {
            tx_or_package_hash = GetPackageHash(m_subpackage.m_changeset->GetAddedTxns());
            log_string += strprintf("New package %s with %lu txs, fees=%s, vsize=%s",
                                    tx_or_package_hash.ToString(),
                                    m_subpackage.m_changeset->GetTxCount(),
                                    feerate.fee,
                                    feerate.size);

        }
        LogDebug(BCLog::MEMPOOL, "%s\n", log_string);
        TRACEPOINT(mempool, replaced,
                it->GetTx().GetHash().data(),
                it->GetTxSize(),
                it->GetFee(),
                std::chrono::duration_cast<std::chrono::duration<std::uint64_t>>(it->GetTime()).count(),
                tx_or_package_hash.data(),
                feerate.size,
                feerate.fee,
                replaced_with_tx
        );
        m_subpackage.m_replaced_transactions.push_back(it->GetSharedTx());
    }
    m_subpackage.m_changeset->Apply();
    m_subpackage.m_changeset.reset();
}

bool MemPoolAccept::SubmitPackage(const ATMPArgs& args, std::vector<Workspace>& workspaces,
                                  PackageValidationState& package_state,
                                  std::map<Wtxid, MempoolAcceptResult>& results)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);
    // Sanity check: none of the transactions should be in the mempool, and none of the transactions
    // should have a same-txid-different-witness equivalent in the mempool.
    assert(std::all_of(workspaces.cbegin(), workspaces.cend(), [this](const auto& ws) { return !m_pool.exists(ws.m_ptx->GetHash()); }));

    bool all_submitted = true;
    FinalizeSubpackage(args);
    // ConsensusScriptChecks adds to the script cache and is therefore consensus-critical;
    // CheckInputsFromMempoolAndCache asserts that transactions only spend coins available from the
    // mempool or UTXO set. Submit each transaction to the mempool immediately after calling
    // ConsensusScriptChecks to make the outputs available for subsequent transactions.
    for (Workspace& ws : workspaces) {
        if (!ConsensusScriptChecks(ws)) {
            results.emplace(ws.m_ptx->GetWitnessHash(), MempoolAcceptResult::Failure(ws.m_state));
            // Since PolicyScriptChecks() passed, this should never fail.
            Assume(false);
            all_submitted = false;
            package_state.Invalid(PackageValidationResult::PCKG_MEMPOOL_ERROR,
                                  strprintf("BUG! PolicyScriptChecks succeeded but ConsensusScriptChecks failed: %s",
                                            ws.m_ptx->GetHash().ToString()));
        }
        // Remove first failing tx and all subsequent in package
        if (!all_submitted) {
            if (!m_subpackage.m_changeset) m_subpackage.m_changeset = m_pool.GetChangeSet();
            m_subpackage.m_changeset->StageRemoval(m_pool.GetIter(ws.m_ptx->GetHash()).value());
        }
    }
    if (!all_submitted) {
        Assume(m_subpackage.m_changeset);
        // This code should be unreachable; it's here as belt-and-suspenders
        // to try to ensure we have no consensus-invalid transactions in the
        // mempool.
        m_subpackage.m_changeset->Apply();
        m_subpackage.m_changeset.reset();
        return false;
    }

    std::vector<Wtxid> all_package_wtxids;
    all_package_wtxids.reserve(workspaces.size());
    std::transform(workspaces.cbegin(), workspaces.cend(), std::back_inserter(all_package_wtxids),
                   [](const auto& ws) { return ws.m_ptx->GetWitnessHash(); });

    if (!m_subpackage.m_replaced_transactions.empty()) {
        LogDebug(BCLog::MEMPOOL, "replaced %u mempool transactions with %u new one(s) for %s additional fees, %d delta bytes\n",
                 m_subpackage.m_replaced_transactions.size(), workspaces.size(),
                 m_subpackage.m_total_modified_fees - m_subpackage.m_conflicting_fees,
                 m_subpackage.m_total_vsize - static_cast<int>(m_subpackage.m_conflicting_size));
    }

    // Add successful results. The returned results may change later if LimitMempoolSize() evicts them.
    for (Workspace& ws : workspaces) {
        auto iter = m_pool.GetIter(ws.m_ptx->GetHash());
        Assume(iter.has_value());
        const auto effective_feerate = args.m_package_feerates ? ws.m_package_feerate :
            CFeeRate{ws.m_modified_fees, static_cast<int32_t>(ws.m_vsize)};
        const auto effective_feerate_wtxids = args.m_package_feerates ? all_package_wtxids :
            std::vector<Wtxid>{ws.m_ptx->GetWitnessHash()};
        results.emplace(ws.m_ptx->GetWitnessHash(),
                        MempoolAcceptResult::Success(std::move(m_subpackage.m_replaced_transactions), ws.m_vsize,
                                         ws.m_base_fees, effective_feerate, effective_feerate_wtxids));
        if (!m_pool.m_opts.signals) continue;
        const CTransaction& tx = *ws.m_ptx;
        const auto tx_info = NewMempoolTransactionInfo(ws.m_ptx, ws.m_base_fees,
                                                       ws.m_vsize, (*iter)->GetHeight(),
                                                       args.m_bypass_limits, args.m_package_submission,
                                                       IsCurrentForFeeEstimation(m_active_chainstate),
                                                       m_pool.HasNoInputsOf(tx));
        m_pool.m_opts.signals->TransactionAddedToMempool(tx_info, m_pool.GetAndIncrementSequence());
    }
    return all_submitted;
}

MempoolAcceptResult MemPoolAccept::AcceptSingleTransactionInternal(const CTransactionRef& ptx, ATMPArgs& args)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);

    Workspace ws(ptx);
    const std::vector<Wtxid> single_wtxid{ws.m_ptx->GetWitnessHash()};

    if (!PreChecks(args, ws)) {
        if (ws.m_state.GetResult() == TxValidationResult::TX_RECONSIDERABLE) {
            // Failed for fee reasons. Provide the effective feerate and which tx was included.
            return MempoolAcceptResult::FeeFailure(ws.m_state, CFeeRate(ws.m_modified_fees, ws.m_vsize), single_wtxid);
        }
        return MempoolAcceptResult::Failure(ws.m_state);
    }

    if (m_subpackage.m_rbf && !ReplacementChecks(ws)) {
        if (ws.m_state.GetResult() == TxValidationResult::TX_RECONSIDERABLE) {
            // Failed for incentives-based fee reasons. Provide the effective feerate and which tx was included.
            return MempoolAcceptResult::FeeFailure(ws.m_state, CFeeRate(ws.m_modified_fees, ws.m_vsize), single_wtxid);
        }
        return MempoolAcceptResult::Failure(ws.m_state);
    }

    // Check if the transaction would exceed the cluster size limit.
    if (!m_subpackage.m_changeset->CheckMemPoolPolicyLimits()) {
        ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "too-large-cluster", "");
        return MempoolAcceptResult::Failure(ws.m_state);
    }

    // Now that we've verified the cluster limit is respected, we can perform
    // calculations involving the full ancestors of the tx.
    if (ws.m_conflicts.size()) {
        auto ancestors = m_subpackage.m_changeset->CalculateMemPoolAncestors(ws.m_tx_handle);

        // A transaction that spends outputs that would be replaced by it is invalid. Now
        // that we have the set of all ancestors we can detect this
        // pathological case by making sure ws.m_conflicts and this tx's ancestors don't
        // intersect.
        if (const auto err_string{EntriesAndTxidsDisjoint(ancestors, ws.m_conflicts, ptx->GetHash())}) {
            // We classify this as a consensus error because a transaction depending on something it
            // conflicts with would be inconsistent.
            ws.m_state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-spends-conflicting-tx", *err_string);
            return MempoolAcceptResult::Failure(ws.m_state);
        }
    }

    m_subpackage.m_total_vsize = ws.m_vsize;
    m_subpackage.m_total_modified_fees = ws.m_modified_fees;

    // Individual modified feerate exceeded caller-defined max; abort
    if (args.m_client_maxfeerate && CFeeRate(ws.m_modified_fees, ws.m_vsize) > args.m_client_maxfeerate.value()) {
        ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "max feerate exceeded", "");
        return MempoolAcceptResult::Failure(ws.m_state);
    }

    if (!args.m_bypass_limits && m_pool.m_opts.require_standard) {
        Wtxid dummy_wtxid;
        if (!CheckEphemeralSpends(/*package=*/{ptx}, m_pool.m_opts.dust_relay_feerate, m_pool, ws.m_state, dummy_wtxid)) {
            return MempoolAcceptResult::Failure(ws.m_state);
        }
    }

    // Perform the inexpensive checks first and avoid hashing and signature verification unless
    // those checks pass, to mitigate CPU exhaustion denial-of-service attacks.
    if (!PolicyScriptChecks(ws)) return MempoolAcceptResult::Failure(ws.m_state);

    if (!ConsensusScriptChecks(ws)) return MempoolAcceptResult::Failure(ws.m_state);

    const CFeeRate effective_feerate{ws.m_modified_fees, static_cast<int32_t>(ws.m_vsize)};
    // Tx was accepted, but not added
    if (args.m_test_accept) {
        return MempoolAcceptResult::Success(std::move(m_subpackage.m_replaced_transactions), ws.m_vsize,
                                            ws.m_base_fees, effective_feerate, single_wtxid);
    }

    FinalizeSubpackage(args);

    // Limit the mempool, if appropriate.
    if (!args.m_package_submission && !args.m_bypass_limits) {
        LimitMempoolSize(m_pool, m_active_chainstate.CoinsTip());
        // If mempool contents change, then the m_view cache is dirty. Given this isn't a package
        // submission, we won't be using the cache anymore, but clear it anyway for clarity.
        CleanupTemporaryCoins();

        if (!m_pool.exists(ws.m_hash)) {
            // The tx no longer meets our (new) mempool minimum feerate but could be reconsidered in a package.
            ws.m_state.Invalid(TxValidationResult::TX_RECONSIDERABLE, "mempool full");
            return MempoolAcceptResult::FeeFailure(ws.m_state, CFeeRate(ws.m_modified_fees, ws.m_vsize), {ws.m_ptx->GetWitnessHash()});
        }
    }

    if (m_pool.m_opts.signals) {
        const CTransaction& tx = *ws.m_ptx;
        auto iter = m_pool.GetIter(tx.GetHash());
        Assume(iter.has_value());
        const auto tx_info = NewMempoolTransactionInfo(ws.m_ptx, ws.m_base_fees,
                                                       ws.m_vsize, (*iter)->GetHeight(),
                                                       args.m_bypass_limits, args.m_package_submission,
                                                       IsCurrentForFeeEstimation(m_active_chainstate),
                                                       m_pool.HasNoInputsOf(tx));
        m_pool.m_opts.signals->TransactionAddedToMempool(tx_info, m_pool.GetAndIncrementSequence());
    }

    if (!m_subpackage.m_replaced_transactions.empty()) {
        LogDebug(BCLog::MEMPOOL, "replaced %u mempool transactions with 1 new transaction for %s additional fees, %d delta bytes\n",
                 m_subpackage.m_replaced_transactions.size(),
                 ws.m_modified_fees - m_subpackage.m_conflicting_fees,
                 ws.m_vsize - static_cast<int>(m_subpackage.m_conflicting_size));
    }

    return MempoolAcceptResult::Success(std::move(m_subpackage.m_replaced_transactions), ws.m_vsize, ws.m_base_fees,
                                        effective_feerate, single_wtxid);
}

PackageMempoolAcceptResult MemPoolAccept::AcceptMultipleTransactionsInternal(const std::vector<CTransactionRef>& txns, ATMPArgs& args)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(m_pool.cs);

    // These context-free package limits can be done before taking the mempool lock.
    PackageValidationState package_state;
    if (!IsWellFormedPackage(txns, package_state)) return PackageMempoolAcceptResult(package_state, {});

    // The drivechain checks of the mempool (one BMM request per sidechain, deposits that chain on the
    // treasury output) and those of the sidechain rules (one refund per withdrawal, and what a
    // sidechain built on this one adds: sidechain::CheckedAgainstMempool) look at the mempool as it
    // is: a package member would not see the others. Such transactions come one at a time, each
    // paying its own way.
    if (txns.size() > 1) {
        const bool on_sidechain{m_active_chainstate.m_chainman.GetConsensus().sidechain.enabled};
        for (const CTransactionRef& tx : txns) {
            // Shielded transactions too: the nullifiers of a package are not checked against each other.
            const bool drivechain_tx{drivechain::GetBmmRequest(*tx).has_value() || zside::HasShieldedMarker(*tx) ||
                                     (on_sidechain && sidechain::CheckedAgainstMempool(*tx)) ||
                                     std::any_of(tx->vout.begin(), tx->vout.end(), [](const CTxOut& out) { return drivechain::ParseEscrowScript(out.scriptPubKey).has_value(); })};
            if (drivechain_tx) {
                package_state.Invalid(PackageValidationResult::PCKG_POLICY, "package-drivechain-tx");
                return PackageMempoolAcceptResult(package_state, {});
            }
        }
    }

    std::vector<Workspace> workspaces{};
    workspaces.reserve(txns.size());
    std::transform(txns.cbegin(), txns.cend(), std::back_inserter(workspaces),
                   [](const auto& tx) { return Workspace(tx); });
    std::map<Wtxid, MempoolAcceptResult> results;

    // Do all PreChecks first and fail fast to avoid running expensive script checks when unnecessary.
    for (Workspace& ws : workspaces) {
        if (!PreChecks(args, ws)) {
            package_state.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
            // Exit early to avoid doing pointless work. Update the failed tx result; the rest are unfinished.
            results.emplace(ws.m_ptx->GetWitnessHash(), MempoolAcceptResult::Failure(ws.m_state));
            return PackageMempoolAcceptResult(package_state, std::move(results));
        }

        // Individual modified feerate exceeded caller-defined max; abort
        // N.B. this doesn't take into account CPFPs. Chunk-aware validation may be more robust.
        if (args.m_client_maxfeerate && CFeeRate(ws.m_modified_fees, ws.m_vsize) > args.m_client_maxfeerate.value()) {
            // Need to set failure here both individually and at package level
            ws.m_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "max feerate exceeded", "");
            package_state.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
            // Exit early to avoid doing pointless work. Update the failed tx result; the rest are unfinished.
            results.emplace(ws.m_ptx->GetWitnessHash(), MempoolAcceptResult::Failure(ws.m_state));
            return PackageMempoolAcceptResult(package_state, std::move(results));
        }

        // Make the coins created by this transaction available for subsequent transactions in the
        // package to spend. If there are no conflicts within the package, no transaction can spend a coin
        // needed by another transaction in the package. We also need to make sure that no package
        // tx replaces (or replaces the ancestor of) the parent of another package tx. As long as we
        // check these two things, we don't need to track the coins spent.
        // If a package tx conflicts with a mempool tx, PackageRBFChecks() ensures later that any package RBF attempt
        // has *no* in-mempool ancestors, so we don't have to worry about subsequent transactions in
        // same package spending the same in-mempool outpoints. This needs to be revisited for general
        // package RBF.
        m_viewmempool.PackageAddTransaction(ws.m_ptx);
    }

    // At this point we have all in-mempool parents, and we know every transaction's vsize.
    // Run the TRUC checks on the package.
    for (Workspace& ws : workspaces) {
        if (auto err{PackageTRUCChecks(m_pool, ws.m_ptx, ws.m_vsize, txns, ws.m_parents)}) {
            package_state.Invalid(PackageValidationResult::PCKG_POLICY, "TRUC-violation", err.value());
            return PackageMempoolAcceptResult(package_state, {});
        }
    }

    // Transactions must meet two minimum feerates: the mempool minimum fee and min relay fee.
    // For transactions consisting of exactly one child and its parents, it suffices to use the
    // package feerate (total modified fees / total virtual size) to check this requirement.
    // Note that this is an aggregate feerate; this function has not checked that there are transactions
    // too low feerate to pay for themselves, or that the child transactions are higher feerate than
    // their parents. Using aggregate feerate may allow "parents pay for child" behavior and permit
    // a child that is below mempool minimum feerate. To avoid these behaviors, callers of
    // AcceptMultipleTransactions need to restrict txns topology (e.g. to ancestor sets) and check
    // the feerates of individuals and subsets.
    m_subpackage.m_total_vsize = std::accumulate(workspaces.cbegin(), workspaces.cend(), int64_t{0},
        [](int64_t sum, auto& ws) { return sum + ws.m_vsize; });
    m_subpackage.m_total_modified_fees = std::accumulate(workspaces.cbegin(), workspaces.cend(), CAmount{0},
        [](CAmount sum, auto& ws) { return sum + ws.m_modified_fees; });
    const CFeeRate package_feerate(m_subpackage.m_total_modified_fees, m_subpackage.m_total_vsize);
    std::vector<Wtxid> all_package_wtxids;
    all_package_wtxids.reserve(workspaces.size());
    std::transform(workspaces.cbegin(), workspaces.cend(), std::back_inserter(all_package_wtxids),
                   [](const auto& ws) { return ws.m_ptx->GetWitnessHash(); });
    TxValidationState placeholder_state;
    if (args.m_package_feerates &&
        !CheckFeeRate(m_subpackage.m_total_vsize, m_subpackage.m_total_modified_fees, placeholder_state)) {
        package_state.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
        return PackageMempoolAcceptResult(package_state, {{workspaces.back().m_ptx->GetWitnessHash(),
            MempoolAcceptResult::FeeFailure(placeholder_state, CFeeRate(m_subpackage.m_total_modified_fees, m_subpackage.m_total_vsize), all_package_wtxids)}});
    }

    // Apply package mempool RBF checks.
    if (m_subpackage.m_rbf && !PackageRBFChecks(txns, workspaces, package_state)) {
        return PackageMempoolAcceptResult(package_state, std::move(results));
    }

    // Check if the transactions would exceed the cluster size limit.
    if (!m_subpackage.m_changeset->CheckMemPoolPolicyLimits()) {
        package_state.Invalid(PackageValidationResult::PCKG_POLICY, "too-large-cluster", "");
        return PackageMempoolAcceptResult(package_state, std::move(results));
    }

    // Now that we've bounded the resulting possible ancestry count, check package for dust spends
    if (m_pool.m_opts.require_standard) {
        TxValidationState child_state;
        Wtxid child_wtxid;
        if (!CheckEphemeralSpends(txns, m_pool.m_opts.dust_relay_feerate, m_pool, child_state, child_wtxid)) {
            package_state.Invalid(PackageValidationResult::PCKG_TX, "unspent-dust");
            results.emplace(child_wtxid, MempoolAcceptResult::Failure(child_state));
            return PackageMempoolAcceptResult(package_state, std::move(results));
        }
    }

    for (Workspace& ws : workspaces) {
        ws.m_package_feerate = package_feerate;
        if (!PolicyScriptChecks(ws)) {
            // Exit early to avoid doing pointless work. Update the failed tx result; the rest are unfinished.
            package_state.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
            results.emplace(ws.m_ptx->GetWitnessHash(), MempoolAcceptResult::Failure(ws.m_state));
            return PackageMempoolAcceptResult(package_state, std::move(results));
        }
        if (args.m_test_accept) {
            const auto effective_feerate = args.m_package_feerates ? ws.m_package_feerate :
                CFeeRate{ws.m_modified_fees, static_cast<int32_t>(ws.m_vsize)};
            const auto effective_feerate_wtxids = args.m_package_feerates ? all_package_wtxids :
                std::vector<Wtxid>{ws.m_ptx->GetWitnessHash()};
            results.emplace(ws.m_ptx->GetWitnessHash(),
                            MempoolAcceptResult::Success(std::move(m_subpackage.m_replaced_transactions),
                                                         ws.m_vsize, ws.m_base_fees, effective_feerate,
                                                         effective_feerate_wtxids));
        }
    }

    if (args.m_test_accept) return PackageMempoolAcceptResult(package_state, std::move(results));

    if (!SubmitPackage(args, workspaces, package_state, results)) {
        // PackageValidationState filled in by SubmitPackage().
        return PackageMempoolAcceptResult(package_state, std::move(results));
    }

    return PackageMempoolAcceptResult(package_state, std::move(results));
}

void MemPoolAccept::CleanupTemporaryCoins()
{
    // There are 3 kinds of coins in m_view:
    // (1) Temporary coins from the transactions in subpackage, constructed by m_viewmempool.
    // (2) Mempool coins from transactions in the mempool, constructed by m_viewmempool.
    // (3) Confirmed coins fetched from our current UTXO set.
    //
    // (1) Temporary coins need to be removed, regardless of whether the transaction was submitted.
    // If the transaction was submitted to the mempool, m_viewmempool will be able to fetch them from
    // there. If it wasn't submitted to mempool, it is incorrect to keep them - future calls may try
    // to spend those coins that don't actually exist.
    // (2) Mempool coins also need to be removed. If the mempool contents have changed as a result
    // of submitting or replacing transactions, coins previously fetched from mempool may now be
    // spent or nonexistent. Those coins need to be deleted from m_view.
    // (3) Confirmed coins don't need to be removed. The chainstate has not changed (we are
    // holding cs_main and no blocks have been processed) so the confirmed tx cannot disappear like
    // a mempool tx can. The coin may now be spent after we submitted a tx to mempool, but
    // we have already checked that the package does not have 2 transactions spending the same coin
    // and we check whether a mempool transaction spends conflicting coins (CTxMemPool::GetConflictTx).
    // Keeping them in m_view is an optimization to not re-fetch confirmed coins if we later look up
    // inputs for this transaction again.
    for (const auto& outpoint : m_viewmempool.GetNonBaseCoins()) {
        // In addition to resetting m_viewmempool, we also need to manually delete these coins from
        // m_view because it caches copies of the coins it fetched from m_viewmempool previously.
        m_view.Uncache(outpoint);
    }
    // This deletes the temporary and mempool coins.
    m_viewmempool.Reset();
}

PackageMempoolAcceptResult MemPoolAccept::AcceptSubPackage(const std::vector<CTransactionRef>& subpackage, ATMPArgs& args)
{
    AssertLockHeld(::cs_main);
    AssertLockHeld(m_pool.cs);
    auto result = [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_pool.cs) {
        if (subpackage.size() > 1) {
            return AcceptMultipleTransactionsInternal(subpackage, args);
        }
        const auto& tx = subpackage.front();
        ATMPArgs single_args = ATMPArgs::SingleInPackageAccept(args);
        const auto single_res = AcceptSingleTransactionInternal(tx, single_args);
        PackageValidationState package_state_wrapped;
        if (single_res.m_result_type != MempoolAcceptResult::ResultType::VALID) {
            package_state_wrapped.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
        }
        return PackageMempoolAcceptResult(package_state_wrapped, {{tx->GetWitnessHash(), single_res}});
    }();

    // Clean up m_view and m_viewmempool so that other subpackage evaluations don't have access to
    // coins they shouldn't. Keep some coins in order to minimize re-fetching coins from the UTXO set.
    // Clean up package feerate and rbf calculations
    ClearSubPackageState();

    return result;
}

PackageMempoolAcceptResult MemPoolAccept::AcceptPackage(const Package& package, ATMPArgs& args)
{
    Assert(!package.empty());
    AssertLockHeld(cs_main);
    // Used if returning a PackageMempoolAcceptResult directly from this function.
    PackageValidationState package_state_quit_early;

    // There are two topologies we are able to handle through this function:
    // (1) A single transaction
    // (2) A child-with-parents package.
    // Check that the package is well-formed. If it isn't, we won't try to validate any of the
    // transactions and thus won't return any MempoolAcceptResults, just a package-wide error.

    // Context-free package checks.
    if (!IsWellFormedPackage(package, package_state_quit_early)) {
        return PackageMempoolAcceptResult(package_state_quit_early, {});
    }

    if (package.size() > 1 && !IsChildWithParents(package)) {
        // All transactions in the package must be a parent of the last transaction. This is just an
        // opportunity for us to fail fast on a context-free check without taking the mempool lock.
        package_state_quit_early.Invalid(PackageValidationResult::PCKG_POLICY, "package-not-child-with-parents");
        return PackageMempoolAcceptResult(package_state_quit_early, {});
    }

    LOCK(m_pool.cs);
    // Stores results from which we will create the returned PackageMempoolAcceptResult.
    // A result may be changed if a mempool transaction is evicted later due to LimitMempoolSize().
    std::map<Wtxid, MempoolAcceptResult> results_final;
    // Results from individual validation which will be returned if no other result is available for
    // this transaction. "Nonfinal" because if a transaction fails by itself but succeeds later
    // (i.e. when evaluated with a fee-bumping child), the result in this map may be discarded.
    std::map<Wtxid, MempoolAcceptResult> individual_results_nonfinal;
    // Tracks whether we think package submission could result in successful entry to the mempool
    bool quit_early{false};
    std::vector<CTransactionRef> txns_package_eval;
    for (const auto& tx : package) {
        const auto& wtxid = tx->GetWitnessHash();
        const auto& txid = tx->GetHash();
        // There are 3 possibilities: already in mempool, same-txid-diff-wtxid already in mempool,
        // or not in mempool. An already confirmed tx is treated as one not in mempool, because all
        // we know is that the inputs aren't available.
        if (m_pool.exists(wtxid)) {
            // Exact transaction already exists in the mempool.
            // Node operators are free to set their mempool policies however they please, nodes may receive
            // transactions in different orders, and malicious counterparties may try to take advantage of
            // policy differences to pin or delay propagation of transactions. As such, it's possible for
            // some package transaction(s) to already be in the mempool, and we don't want to reject the
            // entire package in that case (as that could be a censorship vector). De-duplicate the
            // transactions that are already in the mempool, and only call AcceptMultipleTransactions() with
            // the new transactions. This ensures we don't double-count transaction counts and sizes when
            // checking ancestor/descendant limits, or double-count transaction fees for fee-related policy.
            const auto& entry{*Assert(m_pool.GetEntry(txid))};
            results_final.emplace(wtxid, MempoolAcceptResult::MempoolTx(entry.GetTxSize(), entry.GetFee()));
        } else if (m_pool.exists(txid)) {
            // Transaction with the same non-witness data but different witness (same txid,
            // different wtxid) already exists in the mempool.
            //
            // We don't allow replacement transactions right now, so just swap the package
            // transaction for the mempool one. Note that we are ignoring the validity of the
            // package transaction passed in.
            // TODO: allow witness replacement in packages.
            const auto& entry{*Assert(m_pool.GetEntry(txid))};
            // Provide the wtxid of the mempool tx so that the caller can look it up in the mempool.
            results_final.emplace(wtxid, MempoolAcceptResult::MempoolTxDifferentWitness(entry.GetTx().GetWitnessHash()));
        } else {
            // Transaction does not already exist in the mempool.
            // Try submitting the transaction on its own.
            const auto single_package_res = AcceptSubPackage({tx}, args);
            const auto& single_res = single_package_res.m_tx_results.at(wtxid);
            if (single_res.m_result_type == MempoolAcceptResult::ResultType::VALID) {
                // The transaction succeeded on its own and is now in the mempool. Don't include it
                // in package validation, because its fees should only be "used" once.
                assert(m_pool.exists(wtxid));
                results_final.emplace(wtxid, single_res);
            } else if (package.size() == 1 || // If there is only one transaction, no need to retry it "as a package"
                       (single_res.m_state.GetResult() != TxValidationResult::TX_RECONSIDERABLE &&
                       single_res.m_state.GetResult() != TxValidationResult::TX_MISSING_INPUTS)) {
                // Package validation policy only differs from individual policy in its evaluation
                // of feerate. For example, if a transaction fails here due to violation of a
                // consensus rule, the result will not change when it is submitted as part of a
                // package. To minimize the amount of repeated work, unless the transaction fails
                // due to feerate or missing inputs (its parent is a previous transaction in the
                // package that failed due to feerate), don't run package validation. Note that this
                // decision might not make sense if different types of packages are allowed in the
                // future.  Continue individually validating the rest of the transactions, because
                // some of them may still be valid.
                quit_early = true;
                package_state_quit_early.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
                individual_results_nonfinal.emplace(wtxid, single_res);
            } else {
                individual_results_nonfinal.emplace(wtxid, single_res);
                txns_package_eval.push_back(tx);
            }
        }
    }

    auto multi_submission_result = quit_early || txns_package_eval.empty() ? PackageMempoolAcceptResult(package_state_quit_early, {}) :
        AcceptSubPackage(txns_package_eval, args);
    PackageValidationState& package_state_final = multi_submission_result.m_state;

    // This is invoked by AcceptSubPackage() already, so this is just here for
    // clarity (since it's not permitted to invoke LimitMempoolSize() while a
    // changeset is outstanding).
    ClearSubPackageState();

    // Make sure we haven't exceeded max mempool size.
    // Package transactions that were submitted to mempool or already in mempool may be evicted.
    // If mempool contents change, then the m_view cache is dirty. It has already been cleared above.
    LimitMempoolSize(m_pool, m_active_chainstate.CoinsTip());

    for (const auto& tx : package) {
        const auto& wtxid = tx->GetWitnessHash();
        if (multi_submission_result.m_tx_results.contains(wtxid)) {
            // We shouldn't have re-submitted if the tx result was already in results_final.
            Assume(!results_final.contains(wtxid));
            // If it was submitted, check to see if the tx is still in the mempool. It could have
            // been evicted due to LimitMempoolSize() above.
            const auto& txresult = multi_submission_result.m_tx_results.at(wtxid);
            if (txresult.m_result_type == MempoolAcceptResult::ResultType::VALID && !m_pool.exists(wtxid)) {
                package_state_final.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
                TxValidationState mempool_full_state;
                mempool_full_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "mempool full");
                results_final.emplace(wtxid, MempoolAcceptResult::Failure(mempool_full_state));
            } else {
                results_final.emplace(wtxid, txresult);
            }
        } else if (const auto it{results_final.find(wtxid)}; it != results_final.end()) {
            // Already-in-mempool transaction. Check to see if it's still there, as it could have
            // been evicted when LimitMempoolSize() was called.
            Assume(it->second.m_result_type != MempoolAcceptResult::ResultType::INVALID);
            Assume(!individual_results_nonfinal.contains(wtxid));
            // Query by txid to include the same-txid-different-witness ones.
            if (!m_pool.exists(tx->GetHash())) {
                package_state_final.Invalid(PackageValidationResult::PCKG_TX, "transaction failed");
                TxValidationState mempool_full_state;
                mempool_full_state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "mempool full");
                // Replace the previous result.
                results_final.erase(wtxid);
                results_final.emplace(wtxid, MempoolAcceptResult::Failure(mempool_full_state));
            }
        } else if (const auto it{individual_results_nonfinal.find(wtxid)}; it != individual_results_nonfinal.end()) {
            Assume(it->second.m_result_type == MempoolAcceptResult::ResultType::INVALID);
            // Interesting result from previous processing.
            results_final.emplace(wtxid, it->second);
        }
    }
    Assume(results_final.size() == package.size());
    return PackageMempoolAcceptResult(package_state_final, std::move(results_final));
}

} // anon namespace

MempoolAcceptResult AcceptToMemoryPool(Chainstate& active_chainstate, const CTransactionRef& tx,
                                       int64_t accept_time, bool bypass_limits, bool test_accept)
{
    AssertLockHeld(::cs_main);
    assert(active_chainstate.GetMempool() != nullptr);
    CTxMemPool& pool{*active_chainstate.GetMempool()};

    std::vector<COutPoint> coins_to_uncache;

    auto args = MemPoolAccept::ATMPArgs::SingleAccept(accept_time, bypass_limits, coins_to_uncache, test_accept);
    MempoolAcceptResult result = MemPoolAccept(pool, active_chainstate).AcceptSingleTransactionAndCleanup(tx, args);

    if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
        // Remove coins that were not present in the coins cache before calling
        // AcceptSingleTransaction(); this is to prevent memory DoS in case we receive a large
        // number of invalid transactions that attempt to overrun the in-memory coins cache
        // (`CCoinsViewCache::cacheCoins`).

        for (const COutPoint& hashTx : coins_to_uncache)
            active_chainstate.CoinsTip().Uncache(hashTx);
        TRACEPOINT(mempool, rejected,
                tx->GetHash().data(),
                result.m_state.GetRejectReason().c_str()
        );
    }
    // After we've (potentially) uncached entries, ensure our coins cache is still within its size limits
    BlockValidationState state_dummy;
    active_chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    return result;
}

PackageMempoolAcceptResult ProcessNewPackage(Chainstate& active_chainstate, CTxMemPool& pool,
                                                   const Package& package, bool test_accept, const std::optional<CFeeRate>& client_maxfeerate)
{
    AssertLockHeld(cs_main);
    assert(!package.empty());
    assert(std::all_of(package.cbegin(), package.cend(), [](const auto& tx){return tx != nullptr;}));

    std::vector<COutPoint> coins_to_uncache;
    auto result = [&]() EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
        AssertLockHeld(cs_main);
        if (test_accept) {
            auto args = MemPoolAccept::ATMPArgs::PackageTestAccept(GetTime(), coins_to_uncache);
            return MemPoolAccept(pool, active_chainstate).AcceptMultipleTransactionsAndCleanup(package, args);
        } else {
            auto args = MemPoolAccept::ATMPArgs::PackageChildWithParents(GetTime(), coins_to_uncache, client_maxfeerate);
            return MemPoolAccept(pool, active_chainstate).AcceptPackage(package, args);
        }
    }();

    // Uncache coins pertaining to transactions that were not submitted to the mempool.
    if (test_accept || result.m_state.IsInvalid()) {
        for (const COutPoint& hashTx : coins_to_uncache) {
            active_chainstate.CoinsTip().Uncache(hashTx);
        }
    }
    // Ensure the coins cache is still within limits.
    BlockValidationState state_dummy;
    active_chainstate.FlushStateToDisk(state_dummy, FlushStateMode::PERIODIC);
    return result;
}

std::optional<drivechain::SideContext> ChainstateManager::SideContext(CAmount& minted, sidechain::StoreOverlay& store) const
{
    if (!GetConsensus().sidechain.enabled) return std::nullopt;
    return drivechain::SideContext{GetConsensus().sidechain, *Assert(m_mainchain), minted, store};
}

CAmount GetBlockSubsidy(int nHeight, const Consensus::Params& consensusParams)
{
    // The coins of a sidechain all come from the mainchain.
    if (consensusParams.sidechain.enabled) return 0;
    int halvings = nHeight / consensusParams.nSubsidyHalvingInterval;
    // Force block reward to zero when right shift is undefined.
    if (halvings >= 64)
        return 0;

    CAmount nSubsidy = 50 * COIN;
    // Subsidy is cut in half every 210,000 blocks which will occur approximately every 4 years.
    nSubsidy >>= halvings;
    return nSubsidy;
}

CoinsViews::CoinsViews(DBParams db_params, CoinsViewOptions options)
    : m_dbview{std::move(db_params), std::move(options)},
      m_catcherview(&m_dbview) {}

void CoinsViews::InitCache(int32_t prevoutfetch_threads)
{
    AssertLockHeld(::cs_main);
    m_cacheview = std::make_unique<CCoinsViewCache>(&m_catcherview);
    auto thread_pool{std::make_shared<ThreadPool>("prevout")};
    if (prevoutfetch_threads > 0) {
        thread_pool->Start(prevoutfetch_threads);
        LogInfo("Block input prevout fetching uses %d additional threads", prevoutfetch_threads);
    }
    m_connect_block_view = std::make_unique<CoinsViewOverlay>(&*m_cacheview, std::move(thread_pool));
}

Chainstate::Chainstate(
    CTxMemPool* mempool,
    BlockManager& blockman,
    ChainstateManager& chainman,
    std::optional<uint256> from_snapshot_blockhash)
    : m_mempool(mempool),
      m_blockman(blockman),
      m_chainman(chainman),
      m_assumeutxo(from_snapshot_blockhash ? Assumeutxo::UNVALIDATED : Assumeutxo::VALIDATED),
      m_from_snapshot_blockhash(from_snapshot_blockhash) {}

fs::path Chainstate::StoragePath() const
{
    fs::path path{m_chainman.m_options.datadir / "chainstate"};
    if (m_from_snapshot_blockhash) {
        path += node::SNAPSHOT_CHAINSTATE_SUFFIX;
    }
    return path;
}

const CBlockIndex* Chainstate::SnapshotBase() const
{
    if (!m_from_snapshot_blockhash) return nullptr;
    if (!m_cached_snapshot_base) m_cached_snapshot_base = Assert(m_chainman.m_blockman.LookupBlockIndex(*m_from_snapshot_blockhash));
    return m_cached_snapshot_base;
}

const CBlockIndex* Chainstate::TargetBlock() const
{
    if (!m_target_blockhash) return nullptr;
    if (!m_cached_target_block) m_cached_target_block = Assert(m_chainman.m_blockman.LookupBlockIndex(*m_target_blockhash));
    return m_cached_target_block;
}

void Chainstate::SetTargetBlock(CBlockIndex* block)
{
    if (block) {
        m_target_blockhash = block->GetBlockHash();
    } else {
        m_target_blockhash.reset();
    }
    m_cached_target_block = block;
}

void Chainstate::SetTargetBlockHash(uint256 block_hash)
{
    m_target_blockhash = block_hash;
    m_cached_target_block = nullptr;
}

void Chainstate::InitCoinsDB(
    size_t cache_size_bytes,
    bool in_memory,
    bool should_wipe)
{
    m_coins_views = std::make_unique<CoinsViews>(
        DBParams{
            .path = StoragePath(),
            .cache_bytes = cache_size_bytes,
            .memory_only = in_memory,
            .wipe_data = should_wipe,
            .obfuscate = true,
            .options = m_chainman.m_options.coins_db},
        m_chainman.m_options.coins_view);

    m_coinsdb_cache_size_bytes = cache_size_bytes;
}

void Chainstate::InitCoinsCache(size_t cache_size_bytes)
{
    AssertLockHeld(::cs_main);
    assert(m_coins_views != nullptr);
    m_coinstip_cache_size_bytes = cache_size_bytes;
    m_coins_views->InitCache(m_chainman.m_options.prevoutfetch_threads_num);
}

// Lock-free: depends on `m_cached_is_ibd`, which is latched by `UpdateIBDStatus()`.
bool ChainstateManager::IsInitialBlockDownload() const noexcept
{
    return m_cached_is_ibd.load(std::memory_order_relaxed);
}

void Chainstate::CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);

    if (this->GetRole().historical) {
        return;
    }

    if (m_chainman.m_best_invalid && m_chainman.m_best_invalid->nChainWork > m_chain.Tip()->nChainWork + (GetBlockProof(*m_chain.Tip()) * 6)) {
        LogWarning("Found invalid chain more than 6 blocks longer than our best chain. This could be due to database corruption or consensus incompatibility with peers.");
        m_chainman.GetNotifications().warningSet(
            kernel::Warning::LARGE_WORK_INVALID_CHAIN,
            _("Warning: Found invalid chain more than 6 blocks longer than our best chain. This could be due to database corruption or consensus incompatibility with peers."));
    } else {
        m_chainman.GetNotifications().warningUnset(kernel::Warning::LARGE_WORK_INVALID_CHAIN);
    }
}

// Called both upon regular invalid block discovery *and* InvalidateBlock
void Chainstate::InvalidChainFound(CBlockIndex* pindexNew)
{
    AssertLockHeld(cs_main);
    if (!m_chainman.m_best_invalid || pindexNew->nChainWork > m_chainman.m_best_invalid->nChainWork) {
        m_chainman.m_best_invalid = pindexNew;
    }
    SetBlockFailureFlags(pindexNew);
    if (m_chainman.m_best_header != nullptr && m_chainman.m_best_header->GetAncestor(pindexNew->nHeight) == pindexNew) {
        m_chainman.RecalculateBestHeader();
    }

    LogInfo("%s: invalid block=%s height=%d log2_work=%f date=%s", __func__,
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      log(pindexNew->nChainWork.getdouble())/log(2.0), FormatISO8601DateTime(pindexNew->GetBlockTime()));
    CBlockIndex *tip = m_chain.Tip();
    assert (tip);
    LogInfo("%s: current best=%s height=%d log2_work=%f date=%s", __func__,
      tip->GetBlockHash().ToString(), m_chain.Height(), log(tip->nChainWork.getdouble())/log(2.0),
      FormatISO8601DateTime(tip->GetBlockTime()));
    CheckForkWarningConditions();
}

// Same as InvalidChainFound, above, except not called directly from InvalidateBlock,
// which does its own setBlockIndexCandidates management.
void Chainstate::InvalidBlockFound(CBlockIndex* pindex, const BlockValidationState& state)
{
    AssertLockHeld(cs_main);
    if (state.GetResult() != BlockValidationResult::BLOCK_MUTATED) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        m_blockman.m_dirty_blockindex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo &txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn &txin : tx.vin) {
            txundo.vprevout.emplace_back();
            bool is_spent = inputs.SpendCoin(txin.prevout, &txundo.vprevout.back());
            assert(is_spent);
        }
    }
    // add outputs
    AddCoins(inputs, tx, nHeight);
}

std::optional<std::pair<ScriptError, std::string>> CScriptCheck::operator()() {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    const CScriptWitness *witness = &ptxTo->vin[nIn].scriptWitness;
    ScriptError error{SCRIPT_ERR_UNKNOWN_ERROR};
    if (VerifyScript(scriptSig, m_tx_out.scriptPubKey, witness, m_flags, CachingTransactionSignatureChecker(ptxTo, nIn, m_tx_out.nValue, cacheStore, *m_signature_cache, *txdata), &error)) {
        return std::nullopt;
    } else {
        auto debug_str = strprintf("input %i of %s (wtxid %s), spending %s:%i", nIn, ptxTo->GetHash().ToString(), ptxTo->GetWitnessHash().ToString(), ptxTo->vin[nIn].prevout.hash.ToString(), ptxTo->vin[nIn].prevout.n);
        return std::make_pair(error, std::move(debug_str));
    }
}

ValidationCache::ValidationCache(const size_t script_execution_cache_bytes, const size_t signature_cache_bytes)
    : m_signature_cache{signature_cache_bytes}
{
    // Setup the salted hasher
    uint256 nonce = GetRandHash();
    // We want the nonce to be 64 bytes long to force the hasher to process
    // this chunk, which makes later hash computations more efficient. We
    // just write our 32-byte entropy twice to fill the 64 bytes.
    m_script_execution_cache_hasher.Write(nonce.begin(), 32);
    m_script_execution_cache_hasher.Write(nonce.begin(), 32);

    const auto [num_elems, approx_size_bytes] = m_script_execution_cache.setup_bytes(script_execution_cache_bytes);
    LogInfo("Using %zu MiB out of %zu MiB requested for script execution cache, able to store %zu elements",
              approx_size_bytes >> 20, script_execution_cache_bytes >> 20, num_elems);
}

/**
 * Check whether all of this transaction's input scripts succeed.
 *
 * This involves ECDSA signature checks so can be computationally intensive. This function should
 * only be called after the cheap sanity checks in CheckTxInputs passed.
 *
 * If pvChecks is not nullptr, script checks are pushed onto it instead of being performed inline. Any
 * script checks which are not necessary (eg due to script execution cache hits) are, obviously,
 * not pushed onto pvChecks/run.
 *
 * Setting cacheSigStore/cacheFullScriptStore to false will remove elements from the corresponding cache
 * which are matched. This is useful for checking blocks where we will likely never need the cache
 * entry again.
 *
 * Note that we may set state.reason to NOT_STANDARD for extra soft-fork flags in flags, block-checking
 * callers should probably reset it to CONSENSUS in such cases.
 *
 * Non-static (and redeclared) in src/test/txvalidationcache_tests.cpp
 */
bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, script_verify_flags flags, bool cacheSigStore,
                       bool cacheFullScriptStore, PrecomputedTransactionData& txdata,
                       ValidationCache& validation_cache,
                       std::vector<CScriptCheck>* pvChecks)
{
    if (tx.IsCoinBase()) return true;

    if (pvChecks) {
        pvChecks->reserve(tx.vin.size());
    }

    // First check if script executions have been cached with the same
    // flags. Note that this assumes that the inputs provided are
    // correct (ie that the transaction hash which is in tx's prevouts
    // properly commits to the scriptPubKey in the inputs view of that
    // transaction).
    uint256 hashCacheEntry;
    CSHA256 hasher = validation_cache.ScriptExecutionCacheHasher();
    hasher.Write(UCharCast(tx.GetWitnessHash().begin()), 32).Write((unsigned char*)&flags, sizeof(flags)).Finalize(hashCacheEntry.begin());
    AssertLockHeld(cs_main); //TODO: Remove this requirement by making CuckooCache not require external locks
    if (validation_cache.m_script_execution_cache.contains(hashCacheEntry, !cacheFullScriptStore)) {
        return true;
    }

    if (!txdata.m_spent_outputs_ready) {
        std::vector<CTxOut> spent_outputs;
        spent_outputs.reserve(tx.vin.size());

        for (const auto& txin : tx.vin) {
            const COutPoint& prevout = txin.prevout;
            const Coin& coin = inputs.AccessCoin(prevout);
            assert(!coin.IsSpent());
            spent_outputs.emplace_back(coin.out);
        }
        txdata.Init(tx, std::move(spent_outputs));
    }
    assert(txdata.m_spent_outputs.size() == tx.vin.size());

    for (unsigned int i = 0; i < tx.vin.size(); i++) {

        // We very carefully only pass in things to CScriptCheck which
        // are clearly committed to by tx' witness hash. This provides
        // a sanity check that our caching is not introducing consensus
        // failures through additional data in, eg, the coins being
        // spent being checked as a part of CScriptCheck.

        // Verify signature
        CScriptCheck check(txdata.m_spent_outputs[i], tx, validation_cache.m_signature_cache, i, flags, cacheSigStore, &txdata);
        if (pvChecks) {
            pvChecks->emplace_back(std::move(check));
        } else if (auto result = check(); result.has_value()) {
            // Tx failures never trigger disconnections/bans.
            // This is so that network splits aren't triggered
            // either due to non-consensus relay policies (such as
            // non-standard DER encodings or non-null dummy
            // arguments) or due to new consensus rules introduced in
            // soft forks.
            if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                return state.Invalid(TxValidationResult::TX_NOT_STANDARD, strprintf("mempool-script-verify-flag-failed (%s)", ScriptErrorString(result->first)), result->second);
            } else {
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strprintf("block-script-verify-flag-failed (%s)", ScriptErrorString(result->first)), result->second);
            }
        }
    }

    if (cacheFullScriptStore && !pvChecks) {
        // We executed all of the provided scripts, and were told to
        // cache the result. Do so now.
        validation_cache.m_script_execution_cache.insert(hashCacheEntry);
    }

    return true;
}

bool FatalError(Notifications& notifications, BlockValidationState& state, const bilingual_str& message)
{
    notifications.fatalError(message);
    return state.Error(message.original);
}

/**
 * Restore the UTXO in a Coin at a given COutPoint
 * @param undo The Coin to be restored.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return A DisconnectResult as an int
 */
int ApplyTxInUndo(Coin&& undo, CCoinsViewCache& view, const COutPoint& out)
{
    bool fClean = true;

    if (view.HaveCoin(out)) fClean = false; // overwriting transaction output

    if (undo.nHeight == 0) {
        // Missing undo metadata (height and coinbase). Older versions included this
        // information only in undo records for the last spend of a transactions'
        // outputs. This implies that it must be present for some other output of the same tx.
        const Coin& alternate = AccessByTxid(view, out.hash);
        if (!alternate.IsSpent()) {
            undo.nHeight = alternate.nHeight;
            undo.fCoinBase = alternate.fCoinBase;
        } else {
            return DISCONNECT_FAILED; // adding output for transaction without known metadata
        }
    }
    // If the coin already exists as an unspent coin in the cache, then the
    // possible_overwrite parameter to AddCoin must be set to true. We have
    // already checked whether an unspent coin exists above using HaveCoin, so
    // we don't need to guess. When fClean is false, an unspent coin already
    // existed and it is an overwrite.
    view.AddCoin(out, std::move(undo), !fClean);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  When FAILED is returned, view is left in an indeterminate state. */
DisconnectResult Chainstate::DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view,
                                             drivechain::SidechainDB* scdb, sidechain::StoreOverlay* side_store)
{
    AssertLockHeld(::cs_main);
    bool fClean = true;

    CBlockUndo blockUndo;
    if (!m_blockman.ReadBlockUndo(blockUndo, *pindex)) {
        LogError("DisconnectBlock(): failure reading undo data\n");
        return DISCONNECT_FAILED;
    }

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        LogError("DisconnectBlock(): block and undo data inconsistent\n");
        return DISCONNECT_FAILED;
    }

    // Ignore blocks that contain transactions which are 'overwritten' by later transactions,
    // unless those are already completely spent.
    // See https://github.com/bitcoin/bitcoin/issues/22596 for additional information.
    // Note: the blocks specified here are different than the ones used in ConnectBlock because DisconnectBlock
    // unwinds the blocks in reverse. As a result, the inconsistency is not discovered until the earlier
    // blocks with the duplicate coinbase transactions are disconnected.
    bool fEnforceBIP30 = !((pindex->nHeight==91722 && pindex->GetBlockHash() == uint256{"00000000000271a2dc26e7667f8419f2e15416dc6955e5a6c6cdf3f2574dd08e"}) ||
                           (pindex->nHeight==91812 && pindex->GetBlockHash() == uint256{"00000000000af0aed4792b1acee3d966af36cf5def14935db8de83d6f9306f2f"}));

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction &tx = *(block.vtx[i]);
        Txid hash = tx.GetHash();
        bool is_coinbase = tx.IsCoinBase();
        bool is_bip30_exception = (is_coinbase && !fEnforceBIP30);

        // Check that all outputs are available and match the outputs in the block itself
        // exactly.
        for (size_t o = 0; o < tx.vout.size(); o++) {
            if (!tx.vout[o].scriptPubKey.IsUnspendable()) {
                COutPoint out(hash, o);
                Coin coin;
                bool is_spent = view.SpendCoin(out, &coin);
                if (!is_spent || tx.vout[o] != coin.out || pindex->nHeight != coin.nHeight || is_coinbase != coin.IsCoinBase()) {
                    if (!is_bip30_exception) {
                        fClean = false; // transaction output mismatch
                    }
                }
            }
        }

        // restore inputs
        if (i > 0) { // not coinbases
            CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size()) {
                LogError("DisconnectBlock(): transaction and undo data inconsistent\n");
                return DISCONNECT_FAILED;
            }
            for (unsigned int j = tx.vin.size(); j > 0;) {
                --j;
                const COutPoint& out = tx.vin[j].prevout;
                int res = ApplyTxInUndo(std::move(txundo.vprevout[j]), view, out);
                if (res == DISCONNECT_FAILED) return DISCONNECT_FAILED;
                fClean = fClean && res != DISCONNECT_UNCLEAN;
            }
            // At this point, all of txundo.vprevout should have been moved out.
        }
    }

    if (scdb && !scdb->GetBlockHash().IsNull()) {
        drivechain::BlockUndo scdb_undo;
        if (scdb->GetBlockHash() != pindex->GetBlockHash()) {
            LogError("DisconnectBlock(): the sidechain database does not belong to the block\n");
            return DISCONNECT_FAILED;
        }
        if (m_blockman.m_drivechain_db->ReadBlockUndo(pindex->GetBlockHash(), scdb_undo)) {
            scdb->DisconnectBlock(scdb_undo);
            // The sidechain state: the earlier values of what the block changed.
            if (side_store) side_store->Revert(scdb_undo.side);
        } else if (m_chainman.GetConsensus().sidechain.enabled) {
            // The state of this chain as a sidechain cannot be derived again in place, as the
            // sidechain database below can: a reorg this deep needs the chainstate built anew.
            LogError("DisconnectBlock(): no drivechain undo data for block %s at height %d (kept for the last %d blocks); restart with -reindex-chainstate\n", pindex->GetBlockHash().ToString(), pindex->nHeight, DRIVECHAIN_UNDO_DEPTH);
            return DISCONNECT_FAILED;
        } else {
            // A block deeper than DRIVECHAIN_UNDO_DEPTH no longer has its undo data: the database
            // before it is derived from the blocks, which also brings back the undo data of the
            // DRIVECHAIN_UNDO_DEPTH blocks below it, for the rest of a deep reorg.
            LogWarning("No drivechain undo data for block %s at height %d: deriving the sidechain database from the blocks", pindex->GetBlockHash().ToString(), pindex->nHeight);
            drivechain::SidechainDB rebuilt;
            const CBlockIndex* genesis{pindex->GetAncestor(0)};
            rebuilt.SetBlockHash(genesis->GetBlockHash());
            if (auto result{RollForwardSidechainDB(rebuilt, genesis, pindex->pprev, pindex->pprev->nHeight - DRIVECHAIN_UNDO_DEPTH)}; !result) {
                if (m_chainman.m_interrupt) {
                    LogInfo("DisconnectBlock(): deriving the sidechain database was interrupted");
                } else {
                    LogError("DisconnectBlock(): failure deriving the sidechain database: %s\n", util::ErrorString(result).original);
                }
                return DISCONNECT_FAILED;
            }
            *scdb = std::move(rebuilt);
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

script_verify_flags GetBlockScriptFlags(const CBlockIndex& block_index, const ChainstateManager& chainman)
{
    const Consensus::Params& consensusparams = chainman.GetConsensus();

    // BIP16 didn't become active until Apr 1 2012 (on mainnet, and
    // retroactively applied to testnet)
    // However, only one historical block violated the P2SH rules (on both
    // mainnet and testnet).
    // Similarly, only one historical block violated the TAPROOT rules on
    // mainnet.
    // For simplicity, always leave P2SH+WITNESS+TAPROOT on except for the two
    // violating blocks.
    script_verify_flags flags{SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_TAPROOT};
    const auto it{consensusparams.script_flag_exceptions.find(*Assert(block_index.phashBlock))};
    if (it != consensusparams.script_flag_exceptions.end()) {
        flags = it->second;
    }

    // Enforce the DERSIG (BIP66) rule
    if (DeploymentActiveAt(block_index, chainman, Consensus::DEPLOYMENT_DERSIG)) {
        flags |= SCRIPT_VERIFY_DERSIG;
    }

    // Enforce CHECKLOCKTIMEVERIFY (BIP65)
    if (DeploymentActiveAt(block_index, chainman, Consensus::DEPLOYMENT_CLTV)) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    // Enforce CHECKSEQUENCEVERIFY (BIP112)
    if (DeploymentActiveAt(block_index, chainman, Consensus::DEPLOYMENT_CSV)) {
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    }

    // Enforce BIP147 NULLDUMMY (activated simultaneously with segwit)
    if (DeploymentActiveAt(block_index, chainman, Consensus::DEPLOYMENT_SEGWIT)) {
        flags |= SCRIPT_VERIFY_NULLDUMMY;
    }

    return flags;
}


/** Apply the effects of this block (with given index) on the UTXO set represented by coins.
 *  Validity checks that depend on the UTXO set are also done; ConnectBlock()
 *  can fail if those validity checks fail (among other reasons). */
bool Chainstate::ConnectBlock(const CBlock& block, BlockValidationState& state, CBlockIndex* pindex,
                               CCoinsViewCache& view, bool fJustCheck, drivechain::SidechainDB* scdb, sidechain::StoreOverlay* side_store)
{
    AssertLockHeld(cs_main);
    assert(pindex);

    uint256 block_hash{block.GetHash()};
    assert(*pindex->phashBlock == block_hash);
    // VerifyDB connects blocks of the chain again, on a sidechain database of its own.
    const bool verifying{scdb && scdb != &m_scdb};

    const auto time_start{SteadyClock::now()};
    const CChainParams& params{m_chainman.GetParams()};

    // Check it again in case a previous version let a bad block in
    // NOTE: We don't currently (re-)invoke ContextualCheckBlock() or
    // ContextualCheckBlockHeader() here. This means that if we add a new
    // consensus rule that is enforced in one of those two functions, then we
    // may have let in a block that violates the rule prior to updating the
    // software, and we would NOT be enforcing the rule here. Fully solving
    // upgrade from one software version to the next after a consensus rule
    // change is potentially tricky and issue-specific (see NeedsRedownload()
    // for one approach that was used for BIP 141 deployment).
    // Also, currently the rule against blocks more than 2 hours in the future
    // is enforced in ContextualCheckBlockHeader(); we wouldn't want to
    // re-enforce that rule here (at least until we make it impossible for
    // the clock to go backward).
    if (!CheckBlock(block, state, params.GetConsensus(), !fJustCheck, !fJustCheck)) {
        if (state.GetResult() == BlockValidationResult::BLOCK_MUTATED) {
            // We don't write down blocks to disk if they may have been
            // corrupted, so this should be impossible unless we're having hardware
            // problems.
            return FatalError(m_chainman.GetNotifications(), state, _("Corrupt block found indicating potential hardware failure."));
        }
        LogError("%s: Consensus::CheckBlock: %s\n", __func__, state.ToString());
        return false;
    }

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == nullptr ? uint256() : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    m_chainman.num_blocks_total++;

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block_hash == params.GetConsensus().hashGenesisBlock) {
        if (!fJustCheck)
            view.SetBestBlock(pindex->GetBlockHash());
        return true;
    }

    const char* script_check_reason;
    if (m_chainman.AssumedValidBlock().IsNull()) {
        script_check_reason = "assumevalid=0 (always verify)";
    } else {
        constexpr int64_t TWO_WEEKS_IN_SECONDS{60 * 60 * 24 * 7 * 2};
        // We've been configured with the hash of a block which has been externally verified to have a valid history.
        // A suitable default value is included with the software and updated from time to time.  Because validity
        //  relative to a piece of software is an objective fact these defaults can be easily reviewed.
        // This setting doesn't force the selection of any particular chain but makes validating some faster by
        //  effectively caching the result of part of the verification.
        BlockMap::const_iterator it{m_blockman.m_block_index.find(m_chainman.AssumedValidBlock())};
        if (it == m_blockman.m_block_index.end()) {
            script_check_reason = "assumevalid hash not in headers";
        } else if (it->second.GetAncestor(pindex->nHeight) != pindex) {
            script_check_reason = (pindex->nHeight > it->second.nHeight) ? "block height above assumevalid height" : "block not in assumevalid chain";
        } else if (m_chainman.m_best_header->GetAncestor(pindex->nHeight) != pindex) {
            script_check_reason = "block not in best header chain";
        } else if (m_chainman.m_best_header->nChainWork < m_chainman.MinimumChainWork()) {
            script_check_reason = "best header chainwork below minimumchainwork";
        } else if (GetBlockProofEquivalentTime(*m_chainman.m_best_header, *pindex, *m_chainman.m_best_header, params.GetConsensus()) <= TWO_WEEKS_IN_SECONDS) {
            script_check_reason = "block too recent relative to best header";
        } else {
            // This block is a member of the assumed verified chain and an ancestor of the best header.
            // Script verification is skipped when connecting blocks under the
            //  assumevalid block. Assuming the assumevalid block is valid this
            //  is safe because block merkle hashes are still computed and checked,
            // Of course, if an assumed valid block is invalid due to false scriptSigs
            //  this optimization would allow an invalid chain to be accepted.
            // The equivalent time check discourages hash power from extorting the network via DOS attack
            //  into accepting an invalid block through telling users they must manually set assumevalid.
            //  Requiring a software change or burying the invalid block, regardless of the setting, makes
            //  it hard to hide the implication of the demand. This also avoids having release candidates
            //  that are hardly doing any signature verification at all in testing without having to
            //  artificially set the default assumed verified block further back.
            // The test against the minimum chain work prevents the skipping when denied access to any chain at
            //  least as good as the expected chain.
            script_check_reason = nullptr;
        }
    }

    const auto time_1{SteadyClock::now()};
    m_chainman.time_check += time_1 - time_start;
    LogDebug(BCLog::BENCH, "    - Sanity checks: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_1 - time_start),
             Ticks<SecondsDouble>(m_chainman.time_check),
             Ticks<MillisecondsDouble>(m_chainman.time_check) / m_chainman.num_blocks_total);

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30, CVE-2012-1909, and https://r6.ca/blog/20120206T005236Z.html for more information.
    // This rule was originally applied to all blocks with a timestamp after March 15, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
    // two in the chain that violate it. This prevents exploiting the issue against nodes during their
    // initial block download.
    bool fEnforceBIP30 = !IsBIP30Repeat(*pindex);

    // Once BIP34 activated it was not possible to create new duplicate coinbases and thus other than starting
    // with the 2 existing duplicate coinbase pairs, not possible to create overwriting txs.  But by the
    // time BIP34 activated, in each of the existing pairs the duplicate coinbase had overwritten the first
    // before the first had been spent.  Since those coinbases are sufficiently buried it's no longer possible to create further
    // duplicate transactions descending from the known pairs either.
    // If we're on the known chain at height greater than where BIP34 activated, we can save the db accesses needed for the BIP30 check.

    // BIP34 requires that a block at height X (block X) has its coinbase
    // scriptSig start with a CScriptNum of X (indicated height X).  The above
    // logic of no longer requiring BIP30 once BIP34 activates is flawed in the
    // case that there is a block X before the BIP34 height of 227,931 which has
    // an indicated height Y where Y is greater than X.  The coinbase for block
    // X would also be a valid coinbase for block Y, which could be a BIP30
    // violation.  An exhaustive search of all mainnet coinbases before the
    // BIP34 height which have an indicated height greater than the block height
    // reveals many occurrences. The 3 lowest indicated heights found are
    // 209,921, 490,897, and 1,983,702 and thus coinbases for blocks at these 3
    // heights would be the first opportunity for BIP30 to be violated.

    // The search reveals a great many blocks which have an indicated height
    // greater than 1,983,702, so we simply remove the optimization to skip
    // BIP30 checking for blocks at height 1,983,702 or higher.  Before we reach
    // that block in another 25 years or so, we should take advantage of a
    // future consensus change to do a new and improved version of BIP34 that
    // will actually prevent ever creating any duplicate coinbases in the
    // future.
    static constexpr int BIP34_IMPLIES_BIP30_LIMIT = 1983702;

    // There is no potential to create a duplicate coinbase at block 209,921
    // because this is still before the BIP34 height and so explicit BIP30
    // checking is still active.

    // The final case is block 176,684 which has an indicated height of
    // 490,897. Unfortunately, this issue was not discovered until about 2 weeks
    // before block 490,897 so there was not much opportunity to address this
    // case other than to carefully analyze it and determine it would not be a
    // problem. Block 490,897 was, in fact, mined with a different coinbase than
    // block 176,684, but it is important to note that even if it hadn't been or
    // is remined on an alternate fork with a duplicate coinbase, we would still
    // not run into a BIP30 violation.  This is because the coinbase for 176,684
    // is spent in block 185,956 in transaction
    // d4f7fbbf92f4a3014a230b2dc70b8058d02eb36ac06b4a0736d9d60eaa9e8781.  This
    // spending transaction can't be duplicated because it also spends coinbase
    // 0328dd85c331237f18e781d692c92de57649529bd5edf1d01036daea32ffde29.  This
    // coinbase has an indicated height of over 4.2 billion, and wouldn't be
    // duplicatable until that height, and it's currently impossible to create a
    // chain that long. Nevertheless we may wish to consider a future soft fork
    // which retroactively prevents block 490,897 from creating a duplicate
    // coinbase. The two historical BIP30 violations often provide a confusing
    // edge case when manipulating the UTXO and it would be simpler not to have
    // another edge case to deal with.

    // testnet3 has no blocks before the BIP34 height with indicated heights
    // post BIP34 before approximately height 486,000,000. After block
    // 1,983,702 testnet3 starts doing unnecessary BIP30 checking again.
    assert(pindex->pprev);
    CBlockIndex* pindexBIP34height = pindex->pprev->GetAncestor(params.GetConsensus().BIP34Height);
    //Only continue to enforce if we're below BIP34 activation height or the block hash at that height doesn't correspond.
    fEnforceBIP30 = fEnforceBIP30 && (!pindexBIP34height || !(pindexBIP34height->GetBlockHash() == params.GetConsensus().BIP34Hash));

    // TODO: Remove BIP30 checking from block height 1,983,702 on, once we have a
    // consensus change that ensures coinbases at those heights cannot
    // duplicate earlier coinbases.
    if (fEnforceBIP30 || pindex->nHeight >= BIP34_IMPLIES_BIP30_LIMIT) {
        for (const auto& tx : block.vtx) {
            for (size_t o = 0; o < tx->vout.size(); o++) {
                if (view.HaveCoin(COutPoint(tx->GetHash(), o))) {
                    state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-BIP30",
                                  "tried to overwrite transaction");
                }
            }
        }
    }

    // Enforce BIP68 (sequence locks)
    int nLockTimeFlags = 0;
    if (DeploymentActiveAt(*pindex, m_chainman, Consensus::DEPLOYMENT_CSV)) {
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
    }

    // Get the script flags for this block
    script_verify_flags flags{GetBlockScriptFlags(*pindex, m_chainman)};

    const auto time_2{SteadyClock::now()};
    m_chainman.time_forks += time_2 - time_1;
    LogDebug(BCLog::BENCH, "    - Fork checks: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<SecondsDouble>(m_chainman.time_forks),
             Ticks<MillisecondsDouble>(m_chainman.time_forks) / m_chainman.num_blocks_total);

    const bool fScriptChecks{!!script_check_reason};
    const kernel::ChainstateRole role{GetRole()};
    if (script_check_reason != m_last_script_check_reason_logged && role.validated && !role.historical) {
        if (fScriptChecks) {
            LogInfo("Enabling script verification at block #%d (%s): %s.",
                    pindex->nHeight, block_hash.ToString(), script_check_reason);
        } else {
            LogInfo("Disabling script verification at block #%d (%s).",
                    pindex->nHeight, block_hash.ToString());
        }
        m_last_script_check_reason_logged = script_check_reason;
    }

    CBlockUndo blockundo;

    // Precomputed transaction data pointers must not be invalidated
    // until after `control` has run the script checks (potentially
    // in multiple threads). Preallocate the vector size so a new allocation
    // doesn't invalidate pointers into the vector, and keep txsdata in scope
    // for as long as `control`.
    std::vector<PrecomputedTransactionData> txsdata(block.vtx.size());
    std::optional<CCheckQueueControl<CScriptCheck>> control;
    if (auto& queue = m_chainman.GetCheckQueue(); queue.HasThreads() && fScriptChecks) control.emplace(queue);

    std::vector<int> prevheights;
    CAmount nFees = 0;
    int nInputs = 0;
    int64_t nSigOpsCost = 0;
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        if (!state.IsValid()) break;
        const CTransaction &tx = *(block.vtx[i]);

        nInputs += tx.vin.size();

        if (!tx.IsCoinBase())
        {
            CAmount txfee = 0;
            TxValidationState tx_state;
            if (!Consensus::CheckTxInputs(tx, tx_state, view, pindex->nHeight, txfee, params.GetConsensus().coinbase_maturity)) {
                // Any transaction validation failure in ConnectBlock is a block consensus failure
                state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                              tx_state.GetRejectReason(),
                              tx_state.GetDebugMessage() + " in transaction " + tx.GetHash().ToString());
                break;
            }
            nFees += txfee;
            if (!MoneyRange(nFees)) {
                state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-accumulated-fee-outofrange",
                              "accumulated fee in the block out of range");
                break;
            }

            // Check that transaction is BIP68 final
            // BIP68 lock checks (as opposed to nLockTime checks) must
            // be in ConnectBlock because they require the UTXO set
            prevheights.resize(tx.vin.size());
            for (size_t j = 0; j < tx.vin.size(); j++) {
                prevheights[j] = view.AccessCoin(tx.vin[j].prevout).nHeight;
            }

            if (!SequenceLocks(tx, nLockTimeFlags, prevheights, *pindex)) {
                state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-nonfinal",
                              "contains a non-BIP68-final transaction " + tx.GetHash().ToString());
                break;
            }
        }

        // GetTransactionSigOpCost counts 3 types of sigops:
        // * legacy (always)
        // * p2sh (when P2SH enabled in flags and excludes coinbase)
        // * witness (when witness enabled in flags and excludes coinbase)
        nSigOpsCost += GetTransactionSigOpCost(tx, view, flags);
        if (nSigOpsCost > params.GetConsensus().MaxBlockSigOpsCost()) {
            state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-sigops", "too many sigops");
            break;
        }

        if (!tx.IsCoinBase() && fScriptChecks)
        {
            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            bool tx_ok;
            TxValidationState tx_state;
            // If CheckInputScripts is called with a pointer to a checks vector, the resulting checks are appended to it. In that case
            // they need to be added to control which runs them asynchronously. Otherwise, CheckInputScripts runs the checks before returning.
            if (control) {
                std::vector<CScriptCheck> vChecks;
                tx_ok = CheckInputScripts(tx, tx_state, view, flags, fCacheResults, fCacheResults, txsdata[i], m_chainman.m_validation_cache, &vChecks);
                if (tx_ok) control->Add(std::move(vChecks));
            } else {
                tx_ok = CheckInputScripts(tx, tx_state, view, flags, fCacheResults, fCacheResults, txsdata[i], m_chainman.m_validation_cache);
            }
            if (!tx_ok) {
                // Any transaction validation failure in ConnectBlock is a block consensus failure
                state.Invalid(BlockValidationResult::BLOCK_CONSENSUS,
                              tx_state.GetRejectReason(), tx_state.GetDebugMessage());
                break;
            }
        }

        // A transaction without inputs (allowed by some sidechains) does not spend anything, so
        // nothing stops it from being included twice: its outputs would then overwrite unspent
        // coins, which AddCoin refuses by throwing. Refuse the block instead.
        if (!tx.IsCoinBase() && tx.vin.empty()) {
            bool overwrite{false};
            for (size_t o = 0; o < tx.vout.size(); ++o) {
                if (view.HaveCoin(COutPoint{tx.GetHash(), static_cast<uint32_t>(o)})) {
                    overwrite = true;
                    break;
                }
            }
            if (overwrite) {
                state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-BIP30",
                              "transaction without inputs overwrites an unspent output: " + tx.GetHash().ToString());
                break;
            }
        }

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.emplace_back();
        }
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight);
    }
    const auto time_3{SteadyClock::now()};
    m_chainman.time_connect += time_3 - time_2;
    LogDebug(BCLog::BENCH, "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs (%.2fms/blk)]\n", (unsigned)block.vtx.size(),
             Ticks<MillisecondsDouble>(time_3 - time_2), Ticks<MillisecondsDouble>(time_3 - time_2) / block.vtx.size(),
             nInputs <= 1 ? 0 : Ticks<MillisecondsDouble>(time_3 - time_2) / (nInputs - 1),
             Ticks<SecondsDouble>(m_chainman.time_connect),
             Ticks<MillisecondsDouble>(m_chainman.time_connect) / m_chainman.num_blocks_total);

    if (control) {
        auto parallel_result = control->Complete();
        if (parallel_result.has_value() && state.IsValid()) {
            state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, strprintf("block-script-verify-flag-failed (%s)", ScriptErrorString(parallel_result->first)), parallel_result->second);
        }
    }

    // Drivechain rules: sidechain proposals, deposits, withdrawals and blind merged mining.
    // Without a database of the caller's, the block is checked against the chainstate's, which is
    // given back as it was however the block turns out (rather than checked against a copy, which
    // would cost as much as the database is large).
    const bool scdb_temporary{!scdb};
    if (!scdb) scdb = &m_scdb;
    drivechain::BlockUndo scdb_undo;
    std::vector<drivechain::Deposit> scdb_deposits;
    CAmount minted{0};
    // The caller's database is updated in place: if the block stops anywhere after, it is taken
    // back by the undo data so far, which is complete at every point of failure.
    struct ScdbRollback {
        drivechain::SidechainDB* db{nullptr};
        const drivechain::BlockUndo* undo{nullptr};
        uint256 hash;
        ScdbRollback() = default;
        ScdbRollback(const ScdbRollback&) = delete;
        ScdbRollback& operator=(const ScdbRollback&) = delete;
        ~ScdbRollback()
        {
            if (!db) return;
            db->DisconnectBlock(*undo);
            db->SetBlockHash(hash);
        }
    } scdb_rollback;
    // The block's changes to the state of this chain as a sidechain: an overlay of their own over
    // where the caller keeps them (or the chainstate's store, only to check against). It notes the
    // earlier value of what it changes: the undo data. Merged at the end if all went well; dropped
    // otherwise, which takes them back.
    sidechain::StoreOverlay& side_base{side_store ? *side_store : SideCache()};
    sidechain::StoreOverlay block_store{side_base, /*journal=*/true};
    if (state.IsValid()) {
        std::string reject_reason;
        const auto side{m_chainman.SideContext(minted, block_store)};
        const uint256 scdb_hash{scdb->GetBlockHash()};
        // A sidechain database that has not seen a block yet is empty and fits any block.
        if (scdb->GetBlockHash().IsNull()) scdb->SetBlockHash(hashPrevBlock);
        if (scdb->GetBlockHash() != hashPrevBlock) {
            // When only checking a block this is the caller's problem, not a sign of corrupt data.
            if (fJustCheck) {
                state.Error("the sidechain database does not match the block being checked");
                return false;
            }
            return FatalError(m_chainman.GetNotifications(), state, _("The sidechain database does not match the block being connected."));
        }
        // On regtest the block is also applied to a copy, which has to come out the same: what is
        // applied in place is what a copy would have become.
        std::optional<drivechain::SidechainDB> scdb_check;
        if (params.DefaultConsistencyChecks()) scdb_check.emplace(*scdb);
        // Field by field: assigning a temporary would run its destructor, which rolls back.
        scdb_rollback.undo = &scdb_undo;
        scdb_rollback.hash = scdb_hash;
        scdb_rollback.db = scdb;
        if (!scdb->ConnectBlock(block, pindex->nHeight, params.GetConsensus().drivechain, scdb_undo, &scdb_deposits, reject_reason, side ? &*side : nullptr)) {
            // The rules of the sidechain are checked against the record of the mainchain, which can
            // change: the follower takes the failure back if the block's commitment comes back.
            // A rule broken whatever the mainchain did is no failure of the record's.
            // A signature the node could not check when it took the header (unchecked), and that fails:
            // the failure of the copy it has, not of the block (ChainHooks::ExtensionFailure). In memory
            // only, taken back by another extension, and no fault of the peers that sent it: it was not
            // verifiable when they did.
            const bool extension{!fJustCheck && pindex->m_unchecked && sidechain::Hooks().ExtensionFailure(reject_reason)};
            if (extension) {
                if (const auto found{m_chainman.m_unchecked.find(pindex)}; found != m_chainman.m_unchecked.end()) found->second.extension_failed = true;
            } else if (side && side->failed && !fJustCheck && !verifying && !sidechain::RecordIndependent(reject_reason)) {
                m_chainman.m_mainchain->NoteFailure(block_hash, sidechain::Mainchain::Failure::RECORD);
            }
            // What is not on record yet is no fault of the peer that sent the block. Nor, to VerifyDB,
            // is a block of the chain the record does not agree with any more a sign of a corrupt database.
            const bool not_yet{sidechain::Hooks().IsWaitReason(reject_reason) || reject_reason == "bmm-unknown" || reject_reason == "bad-sc-main-unknown" || (verifying && side && side->failed)};
            // Nor, where record failures are pardoned, a rule that reads the record: the peer's may
            // say otherwise (after a mainchain reorg, the nodes that connected such a block again
            // disconnected the peer that had relayed it before).
            const bool pardoned{side && side->failed && !sidechain::RecordIndependent(reject_reason) && sidechain::Hooks().PardonRecordFailures()};
            state.Invalid(not_yet || pardoned || extension ? BlockValidationResult::BLOCK_TIME_FUTURE : BlockValidationResult::BLOCK_CONSENSUS, reject_reason);
        } else if (scdb_check) {
            CAmount check_minted{0};
            sidechain::StoreOverlay check_store{side_base, /*journal=*/true};
            const auto check_side{m_chainman.SideContext(check_minted, check_store)};
            drivechain::BlockUndo check_undo;
            std::vector<drivechain::Deposit> check_deposits;
            std::string check_reason;
            assert(scdb_check->ConnectBlock(block, pindex->nHeight, params.GetConsensus().drivechain, check_undo, &check_deposits, check_reason, check_side ? &*check_side : nullptr));
            assert(*scdb_check == *scdb && check_minted == minted);
            assert(check_store.Changes() == block_store.Changes() && check_undo.side == scdb_undo.side);
        }
    }

    // On a sidechain the coinbase also creates the coins of deposits and refunds.
    CAmount blockReward = nFees + GetBlockSubsidy(pindex->nHeight, params.GetConsensus()) + minted;
    if (block.vtx[0]->GetValueOut() > blockReward && state.IsValid()) {
        state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-cb-amount",
                      strprintf("coinbase pays too much (actual=%d vs limit=%d)", block.vtx[0]->GetValueOut(), blockReward));
    }

    if (!state.IsValid()) {
        LogInfo("Block validation error: %s", state.ToString());
        return false;
    }
    const auto time_4{SteadyClock::now()};
    m_chainman.time_verify += time_4 - time_2;
    LogDebug(BCLog::BENCH, "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs (%.2fms/blk)]\n", nInputs - 1,
             Ticks<MillisecondsDouble>(time_4 - time_2),
             nInputs <= 1 ? 0 : Ticks<MillisecondsDouble>(time_4 - time_2) / (nInputs - 1),
             Ticks<SecondsDouble>(m_chainman.time_verify),
             Ticks<MillisecondsDouble>(m_chainman.time_verify) / m_chainman.num_blocks_total);

    if (fJustCheck) {
        // The rollback gives a temporary database back.
        if (!scdb_temporary) scdb_rollback.db = nullptr;
        return true;
    }

    // The block of an unchecked header, stored apart, passed every rule: it goes to the block files
    // now, before its undo data (which goes with the block file).
    if (node::BlockManager::IsVolatile(pindex->GetBlockPos()) && !m_chainman.PromoteUncheckedBlock(block, *pindex)) {
        return FatalError(m_chainman.GetNotifications(), state, _("Failed to write block."));
    }
    if (!m_blockman.WriteBlockUndo(blockundo, state, *pindex)) {
        return false;
    }
    // VerifyDB connects the blocks of the chain again, against the record of the mainchain as it is
    // now: if a commitment moved, that is not what the block did when it was connected, which the
    // undo data has to keep to take the block back.
    if (!verifying && !m_blockman.m_drivechain_db->WriteBlock(block_hash, pindex->nHeight, scdb_undo, scdb_deposits, *scdb)) {
        return FatalError(m_chainman.GetNotifications(), state, _("Failed to write drivechain undo data."));
    }

    const auto time_5{SteadyClock::now()};
    m_chainman.time_undo += time_5 - time_4;
    LogDebug(BCLog::BENCH, "    - Write undo data: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_5 - time_4),
             Ticks<SecondsDouble>(m_chainman.time_undo),
             Ticks<MillisecondsDouble>(m_chainman.time_undo) / m_chainman.num_blocks_total);

    if (!pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        m_blockman.m_dirty_blockindex.insert(pindex);
    }

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    const auto time_6{SteadyClock::now()};
    m_chainman.time_index += time_6 - time_5;
    LogDebug(BCLog::BENCH, "    - Index writing: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_6 - time_5),
             Ticks<SecondsDouble>(m_chainman.time_index),
             Ticks<MillisecondsDouble>(m_chainman.time_index) / m_chainman.num_blocks_total);

    TRACEPOINT(validation, block_connected,
        block_hash.data(),
        pindex->nHeight,
        block.vtx.size(),
        nInputs,
        nSigOpsCost,
        Ticks<std::chrono::nanoseconds>(time_5 - time_start)
    );

    if (side_store) {
        // (What the mempool checks again: RemoveStaleDrivechainTxs.)
        if (side_store == m_side_cache.get()) NoteSideChanges(block_store.Changes());
        block_store.MergeInto(*side_store);
    }
    if (!scdb_temporary) scdb_rollback.db = nullptr;
    return true;
}

CoinsCacheSizeState Chainstate::GetCoinsCacheSizeState()
{
    AssertLockHeld(::cs_main);
    return this->GetCoinsCacheSizeState(
        m_coinstip_cache_size_bytes,
        m_mempool ? m_mempool->m_opts.max_size_bytes : 0);
}

CoinsCacheSizeState Chainstate::GetCoinsCacheSizeState(
    size_t max_coins_cache_size_bytes,
    size_t max_mempool_size_bytes)
{
    AssertLockHeld(::cs_main);
    const int64_t nMempoolUsage = m_mempool ? m_mempool->DynamicMemoryUsage() : 0;
    int64_t cacheSize = CoinsTip().DynamicMemoryUsage();
    int64_t nTotalSpace =
        max_coins_cache_size_bytes + std::max<int64_t>(int64_t(max_mempool_size_bytes) - nMempoolUsage, 0);

    if (cacheSize > nTotalSpace) {
        LogInfo("Cache size (%s) exceeds total space (%s)\n", cacheSize, nTotalSpace);
        return CoinsCacheSizeState::CRITICAL;
    } else if (cacheSize > LargeCoinsCacheThreshold(nTotalSpace)) {
        return CoinsCacheSizeState::LARGE;
    }
    return CoinsCacheSizeState::OK;
}

bool Chainstate::FlushStateToDisk(
    BlockValidationState &state,
    FlushStateMode mode,
    int nManualPruneHeight)
{
    LOCK(cs_main);
    assert(this->CanFlushToDisk());
    std::set<int> setFilesToPrune;
    bool full_flush_completed = false;

    [[maybe_unused]] const size_t coins_count{CoinsTip().GetCacheSize()};
    [[maybe_unused]] const size_t coins_mem_usage{CoinsTip().DynamicMemoryUsage()};

    try {
    {
        bool fFlushForPrune = false;

        CoinsCacheSizeState cache_state = GetCoinsCacheSizeState();
        if (m_blockman.IsPruneMode() && (m_blockman.m_check_for_pruning || nManualPruneHeight > 0) && m_chainman.m_blockman.m_blockfiles_indexed) {
            // make sure we don't prune above any of the prune locks bestblocks
            // pruning is height-based
            int last_prune{m_chain.Height()}; // last height we can prune
            std::optional<std::string> limiting_lock; // prune lock that actually was the limiting factor, only used for logging

            for (const auto& prune_lock : m_blockman.m_prune_locks) {
                if (prune_lock.second.height_first == std::numeric_limits<int>::max()) continue;
                // Remove the buffer and one additional block here to get actual height that is outside of the buffer
                const int lock_height{prune_lock.second.height_first - PRUNE_LOCK_BUFFER - 1};
                last_prune = std::max(1, std::min(last_prune, lock_height));
                if (last_prune == lock_height) {
                    limiting_lock = prune_lock.first;
                }
            }

            if (limiting_lock) {
                LogDebug(BCLog::PRUNE, "%s limited pruning to height %d\n", limiting_lock.value(), last_prune);
            }

            if (nManualPruneHeight > 0) {
                LOG_TIME_MILLIS_WITH_CATEGORY("find files to prune (manual)", BCLog::BENCH);

                m_blockman.FindFilesToPruneManual(
                    setFilesToPrune,
                    std::min(last_prune, nManualPruneHeight),
                    *this);
            } else {
                LOG_TIME_MILLIS_WITH_CATEGORY("find files to prune", BCLog::BENCH);

                m_blockman.FindFilesToPrune(setFilesToPrune, last_prune, *this, m_chainman);
                m_blockman.m_check_for_pruning = false;
            }
            if (!setFilesToPrune.empty()) {
                fFlushForPrune = true;
                if (!m_blockman.m_have_pruned) {
                    m_blockman.m_block_tree_db->WriteFlag("prunedblockfiles", true);
                    m_blockman.m_have_pruned = true;
                }
            }
        }
        const auto nNow{NodeClock::now()};
        // The cache is large and we're within 10% and 10 MiB of the limit, but we have time now (not in the middle of a block processing).
        bool fCacheLarge = mode == FlushStateMode::PERIODIC && cache_state >= CoinsCacheSizeState::LARGE;
        // The cache is over the limit, we have to write now.
        bool fCacheCritical = mode == FlushStateMode::IF_NEEDED && cache_state >= CoinsCacheSizeState::CRITICAL;
        // So is the sidechain state's, whose changes are written with the coins: written, but the
        // coins stay in their cache (synced, not emptied), which the sidechain state did not fill.
        const bool side_cache_full{m_side_cache && m_side_cache->Bytes() > SIDE_CACHE_MAX_BYTES && mode != FlushStateMode::NONE};
        // It's been a while since we wrote the block index and chain state to disk. Do this frequently, so we don't need to redownload or reindex after a crash.
        bool fPeriodicWrite = mode == FlushStateMode::PERIODIC && nNow >= m_next_write;
        const auto empty_cache{(mode == FlushStateMode::FORCE_FLUSH) || fCacheLarge || fCacheCritical};
        // Combine all conditions that result in a write to disk.
        bool should_write = (mode == FlushStateMode::FORCE_SYNC) || empty_cache || fPeriodicWrite || fFlushForPrune || side_cache_full;
        // Write blocks, block index and best chain related state to disk.
        if (should_write) {
            LogDebug(BCLog::COINDB, "Writing chainstate to disk: flush mode=%s, prune=%d, large=%d, critical=%d, periodic=%d",
                     FlushStateModeNames[size_t(mode)], fFlushForPrune, fCacheLarge, fCacheCritical, fPeriodicWrite);

            // Ensure we can write block index
            if (!CheckDiskSpace(m_blockman.m_opts.blocks_dir)) {
                return FatalError(m_chainman.GetNotifications(), state, _("Disk space is too low!"));
            }
            {
                LOG_TIME_MILLIS_WITH_CATEGORY("write block and undo data to disk", BCLog::BENCH);

                // First make sure all block and undo data is flushed to disk.
                // TODO: Handle return error, or add detailed comment why it is
                // safe to not return an error upon failure.
                if (!m_blockman.FlushChainstateBlockFile(m_chain.Height())) {
                    LogWarning("%s: Failed to flush block file.\n", __func__);
                }
            }

            // Then update all block file information (which may refer to block and undo files).
            {
                LOG_TIME_MILLIS_WITH_CATEGORY("write block index to disk", BCLog::BENCH);

                m_blockman.WriteBlockIndexDB();
            }
            // Finally remove any pruned files
            if (fFlushForPrune) {
                LOG_TIME_MILLIS_WITH_CATEGORY("unlink pruned files", BCLog::BENCH);

                m_blockman.UnlinkPrunedFiles(setFilesToPrune);
            }

            if (!CoinsTip().GetBestBlock().IsNull()) {
                // Typical Coin structures on disk are around 48 bytes in size.
                // Pushing a new one to the database can cause it to be written
                // twice (once in the log, and once in the tables). This is already
                // an overestimation, as most will delete an existing entry or
                // overwrite one. Still, use a conservative safety factor of 2.
                if (!CheckDiskSpace(m_chainman.m_options.datadir, 48 * 2 * 2 * CoinsTip().GetDirtyCount())) {
                    return FatalError(m_chainman.GetNotifications(), state, _("Disk space is too low!"));
                }
                // Flush the chainstate (which may refer to block index entries).
                empty_cache ? CoinsTip().Flush() : CoinsTip().Sync();
                // Keep a snapshot of the sidechain database that matches the coins on disk.
                if (m_scdb.GetBlockHash() == CoinsTip().GetBestBlock()) {
                    WriteDrivechainState();
                }
                m_last_flushed_block = m_blockman.LookupBlockIndex(CoinsTip().GetBestBlock());
                EraseDrivechainUndo();
                full_flush_completed = true;
                TRACEPOINT(utxocache, flush,
                    int64_t{Ticks<std::chrono::microseconds>(NodeClock::now() - nNow)},
                    (uint32_t)mode,
                    (uint64_t)coins_count,
                    (uint64_t)coins_mem_usage,
                    (bool)fFlushForPrune);
            }
        }

        if (should_write || m_next_write == NodeClock::time_point::max()) {
            constexpr auto range{DATABASE_WRITE_INTERVAL_MAX - DATABASE_WRITE_INTERVAL_MIN};
            m_next_write = FastRandomContext().rand_uniform_delay(NodeClock::now() + DATABASE_WRITE_INTERVAL_MIN, range);
        }
    }
    if (full_flush_completed) {
        if (m_chainman.m_options.signals) {
            m_chainman.m_options.signals->ChainStateFlushed(this->GetRole(), GetLocator(m_last_flushed_block));
        }

        if (!m_chainman.m_interrupt && ShouldCompactChainstate(m_chainman.IsInitialBlockDownload())) {
            try {
                CoinsDB().CompactFullAsync();
            } catch (const std::exception& e) {
                LogWarning("Failed to start chainstate compaction (%s)", e.what());
            }
        }
    }
    } catch (const std::runtime_error& e) {
        return FatalError(m_chainman.GetNotifications(), state, strprintf(_("System error while flushing: %s"), e.what()));
    }
    return true;
}

void Chainstate::ForceFlushStateToDisk(bool wipe_cache)
{
    BlockValidationState state;
    if (!this->FlushStateToDisk(state, wipe_cache ? FlushStateMode::FORCE_FLUSH : FlushStateMode::FORCE_SYNC)) {
        LogWarning("Failed to force flush state (%s)", state.ToString());
    }
}

void Chainstate::PruneAndFlush()
{
    BlockValidationState state;
    m_blockman.m_check_for_pruning = true;
    if (!this->FlushStateToDisk(state, FlushStateMode::NONE)) {
        LogWarning("Failed to flush state (%s)", state.ToString());
    }
}

static void UpdateTipLog(
    const ChainstateManager& chainman,
    const CCoinsViewCache& coins_tip,
    const CBlockIndex* tip,
    const std::string& func_name,
    const std::string& prefix,
    const std::string& warning_messages,
    const bool background_validation) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{

    AssertLockHeld(::cs_main);

    // Disable rate limiting as this may log frequently during IBD.
    LogInfo(util::log::NO_RATE_LIMIT, "%s%s: new best=%s height=%d version=0x%08x log2_work=%f tx=%lu date='%s' progress=%f cache=%.1fMiB(%utxo)%s\n",
                   prefix, func_name,
                   tip->GetBlockHash().ToString(), tip->nHeight, tip->nVersion,
                   log(tip->nChainWork.getdouble()) / log(2.0), tip->m_chain_tx_count,
                   FormatISO8601DateTime(tip->GetBlockTime()),
                   background_validation ? chainman.GetBackgroundVerificationProgress(*tip) : chainman.GuessVerificationProgress(tip),
                   coins_tip.DynamicMemoryUsage() / double(1_MiB),
                   coins_tip.GetCacheSize(),
                   !warning_messages.empty() ? strprintf(" warning='%s'", warning_messages) : "");
}

void Chainstate::UpdateTip(const CBlockIndex* pindexNew)
{
    AssertLockHeld(::cs_main);
    const auto& coins_tip = this->CoinsTip();

    // The remainder of the function isn't relevant if we are not acting on
    // the active chainstate, so return if need be.
    if (this != &m_chainman.ActiveChainstate()) {
        // Only log every so often so that we don't bury log messages at the tip.
        constexpr int BACKGROUND_LOG_INTERVAL = 2000;
        if (pindexNew->nHeight % BACKGROUND_LOG_INTERVAL == 0) {
            UpdateTipLog(m_chainman, coins_tip, pindexNew, __func__, "[background validation] ", "", /*background_validation=*/true);
        }
        return;
    }

    // New best block
    if (m_mempool) {
        m_mempool->AddTransactionsUpdated(1);
    }

    std::vector<bilingual_str> warning_messages;
    if (!m_chainman.IsInitialBlockDownload()) {
        auto bits = m_chainman.m_versionbitscache.CheckUnknownActivations(pindexNew, m_chainman.GetParams());
        for (auto [bit, active] : bits) {
            const bilingual_str warning = strprintf(_("Unknown new rules activated (versionbit %i)"), bit);
            if (active) {
                m_chainman.GetNotifications().warningSet(kernel::Warning::UNKNOWN_NEW_RULES_ACTIVATED, warning);
            } else {
                warning_messages.push_back(warning);
            }
        }
    }
    UpdateTipLog(m_chainman, coins_tip, pindexNew, __func__, "",
                 util::Join(warning_messages, Untranslated(", ")).original, /*background_validation=*/false);
}

/** Disconnect m_chain's tip.
  * After calling, the mempool will be in an inconsistent state, with
  * transactions from disconnected blocks being added to disconnectpool.  You
  * should make the mempool consistent again by calling MaybeUpdateMempoolForReorg.
  * with cs_main held.
  *
  * If disconnectpool is nullptr, then no disconnected transactions are added to
  * disconnectpool (note that the caller is responsible for mempool consistency
  * in any case).
  */
bool Chainstate::DisconnectTip(BlockValidationState& state, DisconnectedBlockTransactions* disconnectpool)
{
    AssertLockHeld(cs_main);
    if (m_mempool) AssertLockHeld(m_mempool->cs);

    CBlockIndex *pindexDelete = m_chain.Tip();
    assert(pindexDelete);
    m_blocks_disconnected = true;
    assert(pindexDelete->pprev);
    // Read block from disk.
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    CBlock& block = *pblock;
    if (!m_blockman.ReadBlock(block, *pindexDelete)) {
        LogError("DisconnectTip(): Failed to read block\n");
        return false;
    }
    // Apply the block atomically to the chain state.
    const auto time_start{SteadyClock::now()};
    {
        CCoinsViewCache view(&CoinsTip());
        assert(view.GetBestBlock() == pindexDelete->GetBlockHash());
        // The sidechain database is taken back in place (it is the last thing DisconnectBlock does);
        // if the coins turn out unclean after all, the block is applied to it again. The sidechain
        // state is undone in an overlay of its own, merged once all went well, dropped otherwise.
        sidechain::StoreOverlay side_store{SideCache(), /*journal=*/false};
        const DisconnectResult disconnected{DisconnectBlock(block, pindexDelete, view, &m_scdb, &side_store)};
        ReleaseSideCursor();
        if (disconnected != DISCONNECT_OK) {
            if (m_chainman.m_interrupt) {
                // Interrupted while deriving the sidechain database for a deep reorg: nothing changed.
                LogInfo("DisconnectTip(): interrupted while disconnecting block %s", pindexDelete->GetBlockHash().ToString());
            } else {
                LogError("DisconnectTip(): DisconnectBlock %s failed\n", pindexDelete->GetBlockHash().ToString());
            }
            if (m_scdb.GetBlockHash() == pindexDelete->pprev->GetBlockHash()) {
                // The coins stay at the block (the view is dropped): the sidechain database goes back
                // to it too, by connecting the block again. That must work, it did before; if it does
                // not, it is taken back to where it was, and the node cannot go on.
                drivechain::BlockUndo undo;
                std::string reject_reason;
                if (!m_scdb.ConnectBlock(block, pindexDelete->nHeight, m_chainman.GetConsensus().drivechain, undo, nullptr, reject_reason)) {
                    m_scdb.DisconnectBlock(undo);
                    m_scdb.SetBlockHash(pindexDelete->pprev->GetBlockHash());
                    LogError("DisconnectTip(): the sidechain database cannot be brought back to %s (%s)\n", pindexDelete->GetBlockHash().ToString(), reject_reason);
                    return FatalError(m_chainman.GetNotifications(), state, _("The sidechain database cannot be brought back to the chain tip."));
                }
            }
            return false;
        }
        view.Flush(/*reallocate_cache=*/false); // local CCoinsViewCache goes out of scope
        NoteSideChanges(side_store.Changes());
        side_store.MergeInto(SideCache());
        // The block's records stay (readers skip blocks out of the active chain; reconnected, it
        // writes them again): erasing them would not be crash-safe, the coins being flushed later.
        // The blocks connected next, at this height and above, keep their undo data until a flush
        // puts them deep enough (a reorg deeper than DRIVECHAIN_UNDO_DEPTH gets here).
        m_drivechain_undo_erased_height = std::min(m_drivechain_undo_erased_height, pindexDelete->nHeight - 1);
    }
    LogDebug(BCLog::BENCH, "- Disconnect block: %.2fms\n",
             Ticks<MillisecondsDouble>(SteadyClock::now() - time_start));

    {
        // Prune locks that began at or after the tip should be moved backward so they get a chance to reorg
        const int max_height_first{pindexDelete->nHeight - 1};
        for (auto& prune_lock : m_blockman.m_prune_locks) {
            if (prune_lock.second.height_first <= max_height_first) continue;

            prune_lock.second.height_first = max_height_first;
            LogDebug(BCLog::PRUNE, "%s prune lock moved back to %d\n", prune_lock.first, max_height_first);
        }
    }

    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FlushStateMode::IF_NEEDED)) {
        return false;
    }

    if (disconnectpool && m_mempool) {
        // Save transactions to re-add to mempool at end of reorg. If any entries are evicted for
        // exceeding memory limits, remove them and their descendants from the mempool.
        for (auto&& evicted_tx : disconnectpool->AddTransactionsFromBlock(block.vtx)) {
            m_mempool->removeRecursive(*evicted_tx, MemPoolRemovalReason::REORG);
        }
    }

    m_chain.SetTip(*pindexDelete->pprev);
    m_chainman.UpdateIBDStatus();

    UpdateTip(pindexDelete->pprev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    if (m_chainman.m_options.signals) {
        m_chainman.m_options.signals->BlockDisconnected(std::move(pblock), pindexDelete);
    }
    return true;
}

struct ConnectedBlock {
    const CBlockIndex* pindex;
    std::shared_ptr<const CBlock> pblock;
};

/**
 * Connect a new block to m_chain. block_to_connect is either nullptr or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 *
 * The block is added to connected_blocks if connection succeeds.
 */
bool Chainstate::ConnectTip(
    BlockValidationState& state,
    CBlockIndex* pindexNew,
    std::shared_ptr<const CBlock> block_to_connect,
    std::vector<ConnectedBlock>& connected_blocks,
    DisconnectedBlockTransactions& disconnectpool)
{
    AssertLockHeld(cs_main);
    if (m_mempool) AssertLockHeld(m_mempool->cs);

    assert(pindexNew->pprev == m_chain.Tip());
    // Read block from disk.
    const auto time_1{SteadyClock::now()};
    if (!block_to_connect) {
        std::shared_ptr<CBlock> pblockNew = std::make_shared<CBlock>();
        if (!m_blockman.ReadBlock(*pblockNew, *pindexNew)) {
            return FatalError(m_chainman.GetNotifications(), state, _("Failed to read block."));
        }
        block_to_connect = std::move(pblockNew);
    } else {
        LogDebug(BCLog::BENCH, "  - Using cached block\n");
        // The block as the node keeps it: with its header's extension (a block that came with
        // another one is stored with the header's).
        if (pindexNew->m_ext && block_to_connect->ext != *pindexNew->m_ext) {
            auto kept{std::make_shared<CBlock>(*block_to_connect)};
            kept->ext = *pindexNew->m_ext;
            block_to_connect = std::move(kept);
        }
    }
    // Apply the block atomically to the chain state.
    const auto time_2{SteadyClock::now()};
    SteadyClock::time_point time_3;
    // When adding aggregate statistics in the future, keep in mind that
    // num_blocks_total may be zero until the ConnectBlock() call below.
    LogDebug(BCLog::BENCH, "  - Load block from disk: %.2fms\n",
             Ticks<MillisecondsDouble>(time_2 - time_1));
    {
        CoinsViewOverlay& view{*m_coins_views->m_connect_block_view};
        const auto reset_guard{view.StartFetching(*block_to_connect)};
        // The sidechain database is updated in place, and taken back if the block fails; a copy
        // is kept only to check that (regtest).
        std::optional<drivechain::SidechainDB> scdb_before;
        if (m_chainman.GetParams().DefaultConsistencyChecks()) scdb_before.emplace(m_scdb);
        bool rv = ConnectBlock(*block_to_connect, state, pindexNew, view, /*fJustCheck=*/false, &m_scdb, &SideCache());
        ReleaseSideCursor();
        if (!rv && scdb_before) assert(m_scdb == *scdb_before);
        if (m_chainman.m_options.signals) {
            m_chainman.m_options.signals->BlockChecked(block_to_connect, state);
        }
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            LogError("%s: ConnectBlock %s failed, %s\n", __func__, pindexNew->GetBlockHash().ToString(), state.ToString());
            return false;
        }
        // An unchecked header is checked now: written to disk from here on (before the chainstate
        // that names it as its tip is).
        m_chainman.ReleaseUnchecked(*pindexNew);
        // (The sidechain undo data of blocks no reorg takes back goes when the chainstate is
        // flushed: see EraseDrivechainUndo.)
        time_3 = SteadyClock::now();
        m_chainman.time_connect_total += time_3 - time_2;
        assert(m_chainman.num_blocks_total > 0);
        LogDebug(BCLog::BENCH, "  - Connect total: %.2fms [%.2fs (%.2fms/blk)]\n",
                 Ticks<MillisecondsDouble>(time_3 - time_2),
                 Ticks<SecondsDouble>(m_chainman.time_connect_total),
                 Ticks<MillisecondsDouble>(m_chainman.time_connect_total) / m_chainman.num_blocks_total);
        view.Flush(/*reallocate_cache=*/false); // No need to reallocate since it only has capacity for 1 block
    }
    const auto time_4{SteadyClock::now()};
    m_chainman.time_flush += time_4 - time_3;
    LogDebug(BCLog::BENCH, "  - Flush: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_4 - time_3),
             Ticks<SecondsDouble>(m_chainman.time_flush),
             Ticks<MillisecondsDouble>(m_chainman.time_flush) / m_chainman.num_blocks_total);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FlushStateMode::IF_NEEDED)) {
        return false;
    }
    const auto time_5{SteadyClock::now()};
    m_chainman.time_chainstate += time_5 - time_4;
    LogDebug(BCLog::BENCH, "  - Writing chainstate: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_5 - time_4),
             Ticks<SecondsDouble>(m_chainman.time_chainstate),
             Ticks<MillisecondsDouble>(m_chainman.time_chainstate) / m_chainman.num_blocks_total);
    // Remove conflicting transactions from the mempool.
    std::vector<RemovedMempoolTransactionInfo> txs_removed_for_block;
    if (m_mempool) {
        txs_removed_for_block = m_mempool->removeForBlock(block_to_connect->vtx);
        disconnectpool.removeForBlock(block_to_connect->vtx);
    }
    // Update m_chain & related variables.
    m_chain.SetTip(*pindexNew);
    m_chainman.UpdateIBDStatus();
    // Not fired while IBD is active. removeForBlock() above still runs.
    if (m_mempool && m_chainman.m_options.signals && !m_chainman.IsInitialBlockDownload()) {
        m_chainman.m_options.signals->MempoolTransactionsRemovedForBlock(block_to_connect, std::move(txs_removed_for_block), pindexNew->nHeight);
    }
    UpdateTip(pindexNew);
    RemoveStaleDrivechainTxs();

    const auto time_6{SteadyClock::now()};
    m_chainman.time_post_connect += time_6 - time_5;
    m_chainman.time_total += time_6 - time_1;
    LogDebug(BCLog::BENCH, "  - Connect postprocess: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_6 - time_5),
             Ticks<SecondsDouble>(m_chainman.time_post_connect),
             Ticks<MillisecondsDouble>(m_chainman.time_post_connect) / m_chainman.num_blocks_total);
    LogDebug(BCLog::BENCH, "- Connect block: %.2fms [%.2fs (%.2fms/blk)]\n",
             Ticks<MillisecondsDouble>(time_6 - time_1),
             Ticks<SecondsDouble>(m_chainman.time_total),
             Ticks<MillisecondsDouble>(m_chainman.time_total) / m_chainman.num_blocks_total);

    // See if this chainstate has reached a target block and can be used to
    // validate an assumeutxo snapshot. If it can, hashing the UTXO database
    // will be slow, and cs_main could remain locked here for several minutes.
    // If the snapshot is validated, the UTXO hash will be saved to
    // this->m_target_utxohash, causing HistoricalChainstate() to return null
    // and this chainstate to no longer be used. ActivateBestChain() will also
    // stop connecting blocks to this chainstate because this->ReachedTarget()
    // will be true and this->setBlockIndexCandidates will not have additional
    // blocks.
    Chainstate& current_cs{m_chainman.CurrentChainstate()};
    m_chainman.MaybeValidateSnapshot(*this, current_cs);

    connected_blocks.emplace_back(pindexNew, std::move(block_to_connect));
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
CBlockIndex* Chainstate::FindMostWorkChain()
{
    AssertLockHeld(::cs_main);
    do {
        CBlockIndex *pindexNew = nullptr;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return nullptr;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        bool fInvalidAncestor = false;
        for (CBlockIndex *pindexTest = pindexNew; pindexTest && !m_chain.Contains(*pindexTest); pindexTest = pindexTest->pprev) {
            assert(pindexTest->HaveNumChainTxs() || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_VALID;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (m_chainman.m_best_invalid == nullptr || pindexNew->nChainWork > m_chainman.m_best_invalid->nChainWork)) {
                    m_chainman.m_best_invalid = pindexNew;
                }
                // Remove the entire chain from the set.
                for (CBlockIndex *pindexFailed = pindexNew; pindexFailed != pindexTest; pindexFailed = pindexFailed->pprev) {
                    // If we're missing data and not a descendant of an invalid block,
                    // then add back to m_blocks_unlinked, so that if the block arrives in the future
                    // we can try adding to setBlockIndexCandidates again.
                    if (fMissingData && !fFailedChain) {
                        // Avoid duplicate entries in m_blocks_unlinked. If the same entry is
                        // processed twice in ReceivedBlockTransactions(), it may be re-added to
                        // setBlockIndexCandidates with a modified nSequenceId, breaking ordering
                        // guarantees and leading to undefined behavior.
                        m_blockman.AddUnlinkedBlock(pindexFailed);
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while(true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
void Chainstate::PruneBlockIndexCandidates() {
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, m_chain.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making index_most_work the active block.
 * pblock is either nullptr or a pointer to a CBlock corresponding to index_most_work.
 *
 * @returns true unless a system error occurred
 */
bool Chainstate::ActivateBestChainStep(BlockValidationState& state, CBlockIndex& index_most_work, const std::shared_ptr<const CBlock>& pblock, bool& fInvalidFound, std::vector<ConnectedBlock>& connected_blocks)
{
    AssertLockHeld(cs_main);
    if (m_mempool) AssertLockHeld(m_mempool->cs);

    const CBlockIndex* pindexOldTip = m_chain.Tip();
    const CBlockIndex* pindexFork = m_chain.FindFork(index_most_work);

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    DisconnectedBlockTransactions disconnectpool{MAX_DISCONNECTED_TX_POOL_BYTES};
    while (m_chain.Tip() && m_chain.Tip() != pindexFork) {
        if (!DisconnectTip(state, &disconnectpool)) {
            // This is likely a fatal error, but keep the mempool consistent,
            // just in case. Only remove from the mempool in this case.
            MaybeUpdateMempoolForReorg(disconnectpool, false);

            // A deep reorg derives the sidechain database from the blocks, which a shutdown
            // interrupts: the node stays on the tip it has, and goes on from there next time.
            if (m_chainman.m_interrupt) return false;

            // If we're unable to disconnect a block during normal operation,
            // then that is a failure of our local system -- we should abort
            // rather than stay on a less work chain.
            FatalError(m_chainman.GetNotifications(), state, _("Failed to disconnect block."));
            return false;
        }
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect (in descending height order).
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != index_most_work.nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, index_most_work.nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex* pindexIter = index_most_work.GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        for (CBlockIndex* pindexConnect : vpindexToConnect | std::views::reverse) {
            if (!ConnectTip(state, pindexConnect, pindexConnect == &index_most_work ? pblock : std::shared_ptr<const CBlock>(), connected_blocks, disconnectpool)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (state.GetResult() != BlockValidationResult::BLOCK_MUTATED) {
                        InvalidChainFound(vpindexToConnect.front());
                    }
                    state = BlockValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    // Make the mempool consistent with the current tip, just in case
                    // any observers try to use it before shutdown.
                    MaybeUpdateMempoolForReorg(disconnectpool, false);
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || m_chain.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        // If any blocks were disconnected, disconnectpool may be non empty.  Add
        // any disconnected transactions back to the mempool.
        MaybeUpdateMempoolForReorg(disconnectpool, true);
    }
    if (m_mempool) m_mempool->check(this->CoinsTip(), this->m_chain.Height() + 1);

    CheckForkWarningConditions();

    return true;
}

static SynchronizationState GetSynchronizationState(bool init, bool blockfiles_indexed)
{
    if (!init) return SynchronizationState::POST_INIT;
    if (!blockfiles_indexed) return SynchronizationState::INIT_REINDEX;
    return SynchronizationState::INIT_DOWNLOAD;
}

void ChainstateManager::UpdateIBDStatus()
{
    AssertLockHeld(cs_main);
    if (!m_cached_is_ibd.load(std::memory_order_relaxed)) return;
    if (m_blockman.LoadingBlocks()) return;
    if (!CurrentChainstate().m_chain.IsTipRecent(MinimumChainWork(), m_options.max_tip_age)) return;
    LogInfo("Leaving InitialBlockDownload (latching to false)");
    m_cached_is_ibd.store(false, std::memory_order_relaxed);
}

std::function<void()> g_test_block_index_use;

bool ChainstateManager::NotifyHeaderTip()
{
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    // (What is notified is read with the lock held: a sidechain's unchecked best header may leave the
    // block index as soon as it is released, EraseUnchecked.)
    int height{0};
    int64_t time{0};
    {
        LOCK(GetMutex());
        CBlockIndex* pindexHeader = m_best_header;

        if (pindexHeader != m_last_notified_header) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload();
            m_last_notified_header = pindexHeader;
            height = pindexHeader->nHeight;
            time = pindexHeader->nTime;
        }
    }
    // Send block tip changed notifications without the lock held
    if (fNotify) {
        GetNotifications().headerTip(GetSynchronizationState(fInitialBlockDownload, m_blockman.m_blockfiles_indexed), height, time, false);
    }
    return fNotify;
}

static void LimitValidationInterfaceQueue(ValidationSignals& signals) LOCKS_EXCLUDED(cs_main) {
    AssertLockNotHeld(cs_main);

    if (signals.CallbacksPending() > 10) {
        signals.SyncWithValidationInterfaceQueue();
    }
}

bool Chainstate::ActivateBestChain(BlockValidationState& state, std::shared_ptr<const CBlock> pblock)
{
    AssertLockNotHeld(m_chainstate_mutex);

    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!
    AssertLockNotHeld(::cs_main);

    // ABC maintains a fair degree of expensive-to-calculate internal state
    // because this function periodically releases cs_main so that it does not lock up other threads for too long
    // during large connects - and to allow for e.g. the callback queue to drain
    // we use m_chainstate_mutex to enforce mutual exclusion so that only one caller may execute this function at a time
    LOCK(m_chainstate_mutex);

    // Belt-and-suspenders check that we aren't attempting to advance the
    // chainstate past the target block.
    if (WITH_LOCK(::cs_main, return m_target_utxohash)) {
        LogError("%s", STR_INTERNAL_BUG("m_target_utxohash is set - this chainstate should not be in operation."));
        return Assume(false);
    }

    CBlockIndex *pindexMostWork = nullptr;
    // When pindexMostWork was found (ChainstateManager::m_unchecked_erasures): it is kept across releases
    // of cs_main, and a sidechain's unchecked headers on the way to it may leave the block index meanwhile
    // (their last holder disconnects: EraseUnchecked). Then it is found again.
    uint64_t most_work_erasures{0};
    CBlockIndex *pindexNewTip = nullptr;
    bool exited_ibd{false};
    do {
        // Block until the validation queue drains. This should largely
        // never happen in normal operation, however may happen during
        // reindex, causing memory blowup if we run too far ahead.
        // Note that if a validationinterface callback ends up calling
        // ActivateBestChain this may lead to a deadlock! We should
        // probably have a DEBUG_LOCKORDER test for this in the future.
        if (m_chainman.m_options.signals) LimitValidationInterfaceQueue(*m_chainman.m_options.signals);

        {
            LOCK(cs_main);
            {
            // Lock transaction pool for at least as long as it takes for connected_blocks to be consumed
            LOCK(MempoolMutex());
            const bool was_in_ibd = m_chainman.IsInitialBlockDownload();
            CBlockIndex* starting_tip = m_chain.Tip();
            bool blocks_connected = false;
            do {
                // We absolutely may not unlock cs_main until we've made forward progress
                // (with the exception of shutdown due to hardware issues, low disk space, etc).
                std::vector<ConnectedBlock> connected_blocks; // Destructed before cs_main is unlocked

                if (pindexMostWork != nullptr && m_chainman.m_unchecked_erasures.load() != most_work_erasures) {
                    pindexMostWork = nullptr;
                }
                if (pindexMostWork == nullptr) {
                    most_work_erasures = m_chainman.m_unchecked_erasures.load();
                    pindexMostWork = FindMostWorkChain();
                }

                // Whether we have anything to do at all.
                if (pindexMostWork == nullptr || pindexMostWork == m_chain.Tip()) {
                    break;
                }

                bool fInvalidFound = false;
                std::shared_ptr<const CBlock> nullBlockPtr;
                // BlockConnected signals must be sent for the original role;
                // in case snapshot validation is completed during ActivateBestChainStep, the
                // result of GetRole() changes from BACKGROUND to NORMAL.
               const ChainstateRole chainstate_role{this->GetRole()};
                if (!ActivateBestChainStep(state, *pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : nullBlockPtr, fInvalidFound, connected_blocks)) {
                    // A system error occurred
                    return false;
                }
                blocks_connected = true;

                if (fInvalidFound) {
                    // Wipe cache, we may need another branch now.
                    pindexMostWork = nullptr;
                }
                pindexNewTip = m_chain.Tip();

                for (auto& [index, block] : std::move(connected_blocks)) {
                    if (m_chainman.m_options.signals) {
                        m_chainman.m_options.signals->BlockConnected(chainstate_role, std::move(Assert(block)), Assert(index));
                    }
                }

                // Break this do-while to ensure we don't advance past the target block.
                if (ReachedTarget()) {
                    break;
                }
            } while (!m_chain.Tip() || (starting_tip && CBlockIndexWorkComparator()(m_chain.Tip(), starting_tip)));
            if (!blocks_connected) return true;

            const CBlockIndex* pindexFork = starting_tip ? m_chain.FindFork(*starting_tip) : nullptr;
            bool still_in_ibd = m_chainman.IsInitialBlockDownload();

            if (was_in_ibd && !still_in_ibd) {
                // Active chainstate has exited IBD.
                exited_ibd = true;
            }

            // Notify external listeners about the new tip.
            // Enqueue while holding cs_main to ensure that UpdatedBlockTip is called in the order in which blocks are connected
            if (this == &m_chainman.ActiveChainstate() && pindexFork != pindexNewTip) {
                // Notify ValidationInterface subscribers
                if (m_chainman.m_options.signals) {
                    m_chainman.m_options.signals->UpdatedBlockTip(pindexNewTip, pindexFork, still_in_ibd);
                }

                if (kernel::IsInterrupted(m_chainman.GetNotifications().blockTip(
                        /*state=*/GetSynchronizationState(still_in_ibd, m_chainman.m_blockman.m_blockfiles_indexed),
                        /*index=*/*pindexNewTip,
                        /*verification_progress=*/m_chainman.GuessVerificationProgress(pindexNewTip))))
                {
                    // Just breaking and returning success for now. This could
                    // be changed to bubble up the kernel::Interrupted value to
                    // the caller so the caller could distinguish between
                    // completed and interrupted operations.
                    break;
                }
            }
            } // release MempoolMutex
            // Notify external listeners about the new tip, even if pindexFork == pindexNewTip.
            if (m_chainman.m_options.signals && this == &m_chainman.ActiveChainstate()) {
                m_chainman.m_options.signals->ActiveTipChange(*Assert(pindexNewTip), m_chainman.IsInitialBlockDownload());
            }
        } // release cs_main
        // When we reach this point, we switched to a new tip (stored in pindexNewTip).

        bool reached_target;
        {
            LOCK(m_chainman.GetMutex());
            if (exited_ibd) {
                // If a background chainstate is in use, we may need to rebalance our
                // allocation of caches once a chainstate exits initial block download.
                m_chainman.MaybeRebalanceCaches();
            }

            // Write changes periodically to disk, after relay.
            if (!FlushStateToDisk(state, FlushStateMode::PERIODIC)) {
                return false;
            }

            reached_target = ReachedTarget();
        }

        if (reached_target) {
            // Chainstate has reached the target block, so exit.
            //
            // Restart indexes so indexes can resync and index new blocks after
            // the target block.
            //
            // This cannot be done while holding cs_main (within
            // MaybeValidateSnapshot) or a cs_main deadlock will occur.
            if (m_chainman.snapshot_download_completed) {
                m_chainman.snapshot_download_completed();
            }
            break;
        }

        // We check interrupt only after giving ActivateBestChainStep a chance to run once so that we
        // never interrupt before connecting the genesis block during LoadChainTip(). Previously this
        // caused an assert() failure during interrupt in such cases as the UTXO DB flushing checks
        // that the best block hash is non-null.
        if (m_chainman.m_interrupt) break;
    } while (pindexNewTip != pindexMostWork);

    m_chainman.CheckBlockIndex();

    return true;
}

bool Chainstate::PreciousBlock(BlockValidationState& state, CBlockIndex* pindex)
{
    return PreciousBlock(state, pindex->GetBlockHash());
}

bool Chainstate::PreciousBlock(BlockValidationState& state, const uint256& hash)
{
    AssertLockNotHeld(m_chainstate_mutex);
    AssertLockNotHeld(::cs_main);
    {
        LOCK(cs_main);
        // (Looked up with the lock held: a sidechain's unchecked header may leave the block index
        // whenever it is released, EraseUnchecked. Gone, there is nothing to do.)
        CBlockIndex* const pindex{m_blockman.LookupBlockIndex(hash)};
        if (!pindex) return true;
        if (pindex->nChainWork < m_chain.Tip()->nChainWork) {
            // Nothing to do, this block is not at the tip.
            return true;
        }
        if (m_chain.Tip()->nChainWork > m_chainman.nLastPreciousChainwork) {
            // The chain has been extended since the last call, reset the counter.
            m_chainman.nBlockReverseSequenceId = -1;
        }
        m_chainman.nLastPreciousChainwork = m_chain.Tip()->nChainWork;
        setBlockIndexCandidates.erase(pindex);
        pindex->nSequenceId = m_chainman.nBlockReverseSequenceId;
        if (m_chainman.nBlockReverseSequenceId > std::numeric_limits<int32_t>::min()) {
            // We can't keep reducing the counter if somebody really wants to
            // call preciousblock 2**31-1 times on the same set of tips...
            m_chainman.nBlockReverseSequenceId--;
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && pindex->HaveNumChainTxs()) {
            setBlockIndexCandidates.insert(pindex);
            PruneBlockIndexCandidates();
        }
    }

    return ActivateBestChain(state, std::shared_ptr<const CBlock>());
}

bool Chainstate::InvalidateBlock(BlockValidationState& state, CBlockIndex* const pindex, bool by_record)
{
    assert(pindex);
    return InvalidateBlock(state, pindex->GetBlockHash(), by_record);
}

bool Chainstate::InvalidateBlock(BlockValidationState& state, const uint256& hash, bool by_record)
{
    AssertLockNotHeld(m_chainstate_mutex);
    AssertLockNotHeld(::cs_main);

    // We do not allow ActivateBestChain() to run while InvalidateBlock() is
    // running, as that could cause the tip to change while we disconnect
    // blocks.
    LOCK(m_chainstate_mutex);

    // We'll be acquiring and releasing cs_main below, to allow the validation
    // callbacks to run. However, we should keep the block index in a
    // consistent state as we disconnect blocks -- in particular we need to
    // add equal-work blocks to setBlockIndexCandidates as we disconnect.
    // To avoid walking the block index repeatedly in search of candidates,
    // build a map once so that we can look up candidate blocks by chain
    // work as we go.
    std::multimap<const arith_uint256, CBlockIndex*> highpow_outofchain_headers;
    // A sidechain's unchecked header may leave the block index whenever cs_main is released
    // (ChainstateManager::EraseUnchecked): pindex itself (until it is found in the active chain, which
    // only checked headers are), and any entry of the map. So, each time the lock is taken again after
    // headers left, pindex is looked up again by hash (gone: there is nothing to mark) and the map
    // built again, rather than rely on the entries kept a while (m_erased_headers).
    CBlockIndex* pindex{nullptr};
    uint64_t erasures{0};
    const auto find_again{[&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        erasures = m_chainman.m_unchecked_erasures.load();
        pindex = m_blockman.LookupBlockIndex(hash);
        highpow_outofchain_headers.clear();
        if (!pindex) return false;
        for (auto& entry : m_blockman.m_block_index) {
            CBlockIndex& candidate = entry.second;
            // We don't need to put anything in our active chain into the
            // multimap, because those candidates will be found and considered
            // as we disconnect.
            // Instead, consider only non-active-chain blocks that score
            // at least as good with CBlockIndexWorkComparator as the new tip.
            if (!m_chain.Contains(candidate) &&
                !CBlockIndexWorkComparator()(&candidate, pindex->pprev) &&
                !(candidate.nStatus & BLOCK_FAILED_VALID)) {
                highpow_outofchain_headers.insert({candidate.nChainWork, &candidate});
            }
        }
        return true;
    }};

    {
        LOCK(cs_main);
        if (!find_again()) return state.Invalid(BlockValidationResult::BLOCK_MISSING_PREV, "block-not-found");
        // Genesis block can't be invalidated
        if (pindex->nHeight == 0) return false;

        // On a sidechain the follower takes back what the record of the mainchain did, not what the operator
        // did: a block the operator marked invalid stays so, whatever the record does later.
        if (m_chainman.m_mainchain && !(by_record && m_chainman.m_mainchain->GetFailure(hash) == sidechain::Mainchain::Failure::MANUAL)) {
            m_chainman.m_mainchain->NoteFailure(hash, by_record ? sidechain::Mainchain::Failure::RECORD : sidechain::Mainchain::Failure::MANUAL);
        }
    }

    CBlockIndex* to_mark_failed = pindex;
    bool pindex_was_in_chain = false;
    int disconnected = 0;

    // Disconnect (descendants of) pindex, and mark them invalid.
    while (true) {
        if (m_chainman.m_interrupt) break;

        // Make sure the queue of validation callbacks doesn't grow unboundedly.
        if (m_chainman.m_options.signals) LimitValidationInterfaceQueue(*m_chainman.m_options.signals);
        TestBlockIndexUse();

        LOCK(cs_main);
        // Lock for as long as disconnectpool is in scope to make sure MaybeUpdateMempoolForReorg is
        // called after DisconnectTip without unlocking in between
        LOCK(MempoolMutex());
        if (m_chainman.m_unchecked_erasures.load() != erasures && !find_again()) {
            // An unchecked header (never in the active chain) that left: nothing to mark.
            return true;
        }
        if (!m_chain.Contains(*pindex)) break;
        pindex_was_in_chain = true;
        CBlockIndex* const disconnected_tip{m_chain.Tip()};

        // ActivateBestChain considers blocks already in m_chain
        // unconditionally valid already, so force disconnect away from it.
        DisconnectedBlockTransactions disconnectpool{MAX_DISCONNECTED_TX_POOL_BYTES};
        bool ret = DisconnectTip(state, &disconnectpool);
        // DisconnectTip will add transactions to disconnectpool.
        // Adjust the mempool to be consistent with the new tip, adding
        // transactions back to the mempool if disconnecting was successful,
        // and we're not doing a very deep invalidation (in which case
        // keeping the mempool up to date is probably futile anyway).
        MaybeUpdateMempoolForReorg(disconnectpool, /* fAddToMempool = */ (++disconnected <= 10) && ret);
        if (!ret) return false;
        CBlockIndex* new_tip{m_chain.Tip()};
        assert(disconnected_tip->pprev == new_tip);

        // We immediately mark the disconnected blocks as invalid.
        // This prevents a case where pruned nodes may fail to invalidateblock
        // and be left unable to start as they have no tip candidates (as there
        // are no blocks that meet the "have data and are not invalid per
        // nStatus" criteria for inclusion in setBlockIndexCandidates).
        disconnected_tip->nStatus |= BLOCK_FAILED_VALID;
        m_blockman.m_dirty_blockindex.insert(disconnected_tip);
        setBlockIndexCandidates.erase(disconnected_tip);
        setBlockIndexCandidates.insert(new_tip);

        // Mark out-of-chain descendants of the invalidated block as invalid
        // Add any equal or more work headers that are not invalidated to setBlockIndexCandidates
        // Recalculate m_best_header if it became invalid.
        auto candidate_it = highpow_outofchain_headers.lower_bound(new_tip->nChainWork);

        const bool best_header_needs_update{m_chainman.m_best_header->GetAncestor(disconnected_tip->nHeight) == disconnected_tip};
        if (best_header_needs_update) {
            // new_tip is definitely still valid at this point, but there may be better ones
            m_chainman.m_best_header = new_tip;
        }

        while (candidate_it != highpow_outofchain_headers.end()) {
            CBlockIndex* candidate{candidate_it->second};
            if (candidate->GetAncestor(disconnected_tip->nHeight) == disconnected_tip) {
                // Children of failed blocks are marked as BLOCK_FAILED_VALID.
                candidate->nStatus |= BLOCK_FAILED_VALID;
                m_blockman.m_dirty_blockindex.insert(candidate);
                // If invalidated, the block is irrelevant for setBlockIndexCandidates
                // and for m_best_header and can be removed from the cache.
                candidate_it = highpow_outofchain_headers.erase(candidate_it);
                continue;
            }
            if (!CBlockIndexWorkComparator()(candidate, new_tip) &&
                candidate->IsValid(BLOCK_VALID_TRANSACTIONS) &&
                candidate->HaveNumChainTxs()) {
                setBlockIndexCandidates.insert(candidate);
                // Do not remove candidate from the highpow_outofchain_headers cache, because it might be a descendant of the block being invalidated
                // which needs to be marked failed later.
            }
            if (best_header_needs_update &&
                m_chainman.m_best_header->nChainWork < candidate->nChainWork) {
                m_chainman.m_best_header = candidate;
            }
            ++candidate_it;
        }

        // Track the last disconnected block to call InvalidChainFound on it.
        to_mark_failed = disconnected_tip;
    }

    m_chainman.CheckBlockIndex();
    TestBlockIndexUse();

    {
        LOCK(cs_main);
        // (to_mark_failed is pindex, or a block that was in the active chain: checked, it stays.)
        if (m_chainman.m_unchecked_erasures.load() != erasures) {
            if (!find_again()) return true;
            if (!pindex_was_in_chain) to_mark_failed = pindex;
        }
        if (m_chain.Contains(*to_mark_failed)) {
            // If the to-be-marked invalid block is in the active chain, something is interfering and we can't proceed.
            return false;
        }

        // Mark pindex as invalid if it never was in the main chain
        if (!pindex_was_in_chain && !(pindex->nStatus & BLOCK_FAILED_VALID)) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            m_blockman.m_dirty_blockindex.insert(pindex);
            setBlockIndexCandidates.erase(pindex);
        }

        // If any new blocks somehow arrived while we were disconnecting
        // (above), then the pre-calculation of what should go into
        // setBlockIndexCandidates may have missed entries. This would
        // technically be an inconsistency in the block index, but if we clean
        // it up here, this should be an essentially unobservable error.
        // Loop back over all block index entries and add any missing entries
        // to setBlockIndexCandidates.
        for (auto& [_, block_index] : m_blockman.m_block_index) {
            if (block_index.IsValid(BLOCK_VALID_TRANSACTIONS) && block_index.HaveNumChainTxs() && !setBlockIndexCandidates.value_comp()(&block_index, m_chain.Tip())) {
                setBlockIndexCandidates.insert(&block_index);
            }
        }

        InvalidChainFound(to_mark_failed);
    }

    // Only notify about a new block tip if the active chain was modified.
    if (pindex_was_in_chain) {
        // Ignoring return value for now, this could be changed to bubble up
        // kernel::Interrupted value to the caller so the caller could
        // distinguish between completed and interrupted operations. It might
        // also make sense for the blockTip notification to have an enum
        // parameter indicating the source of the tip change so hooks can
        // distinguish user-initiated invalidateblock changes from other
        // changes.
        (void)m_chainman.GetNotifications().blockTip(
            /*state=*/GetSynchronizationState(m_chainman.IsInitialBlockDownload(), m_chainman.m_blockman.m_blockfiles_indexed),
            /*index=*/*to_mark_failed->pprev,
            /*verification_progress=*/WITH_LOCK(m_chainman.GetMutex(), return m_chainman.GuessVerificationProgress(to_mark_failed->pprev)));

        // Fire ActiveTipChange now for the current chain tip to make sure clients are notified.
        // ActivateBestChain may call this as well, but not necessarily.
        if (m_chainman.m_options.signals) {
            m_chainman.m_options.signals->ActiveTipChange(*Assert(m_chain.Tip()), m_chainman.IsInitialBlockDownload());
        }
    }
    return true;
}

void Chainstate::SetBlockFailureFlags(CBlockIndex* invalid_block)
{
    AssertLockHeld(cs_main);

    for (auto& [_, block_index] : m_blockman.m_block_index) {
        if (invalid_block != &block_index && block_index.GetAncestor(invalid_block->nHeight) == invalid_block) {
            block_index.nStatus |= BLOCK_FAILED_VALID;
            m_blockman.m_dirty_blockindex.insert(&block_index);
        }
    }
}

void Chainstate::ResetBlockFailureFlags(CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants and ancestors.
    for (auto& [_, block_index] : m_blockman.m_block_index) {
        if ((block_index.nStatus & BLOCK_FAILED_VALID) && (block_index.GetAncestor(nHeight) == pindex || pindex->GetAncestor(block_index.nHeight) == &block_index)) {
            block_index.nStatus &= ~BLOCK_FAILED_VALID;
            m_blockman.m_dirty_blockindex.insert(&block_index);
            if (block_index.IsValid(BLOCK_VALID_TRANSACTIONS) && block_index.HaveNumChainTxs() && setBlockIndexCandidates.value_comp()(m_chain.Tip(), &block_index)) {
                setBlockIndexCandidates.insert(&block_index);
            }
            if (&block_index == m_chainman.m_best_invalid) {
                // Reset invalid block marker if it was pointing to one of those.
                m_chainman.m_best_invalid = nullptr;
            }
            if (m_chainman.m_mainchain) m_chainman.m_mainchain->ForgetFailure(block_index.GetBlockHash());
        }
    }
}

bool Chainstate::ReconsiderRecordFailure(CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);
    if (!m_chainman.m_mainchain || !(pindex->nStatus & BLOCK_FAILED_VALID)) return false;
    sidechain::Mainchain& mainchain{*m_chainman.m_mainchain};
    // The failure of a block is that of the lowest failed block below it: what is built on a failed
    // block is marked failed with it.
    CBlockIndex* root{pindex};
    while (root->pprev && (root->pprev->nStatus & BLOCK_FAILED_VALID)) root = root->pprev;
    if (mainchain.GetFailure(root->GetBlockHash()) != sidechain::Mainchain::Failure::RECORD) return false;
    std::vector<CBlockIndex*> failed;
    for (auto& [_, block_index] : m_blockman.m_block_index) {
        if ((block_index.nStatus & BLOCK_FAILED_VALID) && block_index.GetAncestor(root->nHeight) == root) failed.push_back(&block_index);
    }
    std::sort(failed.begin(), failed.end(), [](const CBlockIndex* a, const CBlockIndex* b) { return a->nHeight < b->nHeight; });
    // A chain order of a sidechain's own may read the record, which changed: the commitment may be at
    // another height now. Computed anew for the block and all above it, parents first (out of the
    // candidates while it changes).
    if (sidechain::Hooks().CustomChainOrder()) {
        std::vector<CBlockIndex*> above;
        for (auto& [_, block_index] : m_blockman.m_block_index) {
            if (block_index.GetAncestor(root->nHeight) == root) above.push_back(&block_index);
        }
        std::sort(above.begin(), above.end(), [](const CBlockIndex* a, const CBlockIndex* b) { return a->nHeight < b->nHeight; });
        for (CBlockIndex* block_index : above) {
            const bool candidate{setBlockIndexCandidates.erase(block_index) > 0};
            block_index->nChainWork = sidechain::Hooks().ChainWork(block_index->pprev, block_index->GetBlockHeader());
            if (candidate) setBlockIndexCandidates.insert(block_index);
        }
    }
    for (CBlockIndex* block_index : failed) {
        // Above a block that stays failed, everything does.
        if (block_index != root && (block_index->pprev->nStatus & BLOCK_FAILED_VALID)) continue;
        if (block_index != root && mainchain.GetFailure(block_index->GetBlockHash()) == sidechain::Mainchain::Failure::MANUAL) continue;
        block_index->nStatus &= ~BLOCK_FAILED_VALID;
        m_blockman.m_dirty_blockindex.insert(block_index);
        if (block_index->IsValid(BLOCK_VALID_TRANSACTIONS) && block_index->HaveNumChainTxs() && setBlockIndexCandidates.value_comp()(m_chain.Tip(), block_index)) {
            setBlockIndexCandidates.insert(block_index);
        }
        if (block_index == m_chainman.m_best_invalid) m_chainman.m_best_invalid = nullptr;
        // Checked again when connected: if it fails, against the record or not, it is noted anew.
        if (mainchain.GetFailure(block_index->GetBlockHash()) == sidechain::Mainchain::Failure::RECORD) mainchain.ForgetFailure(block_index->GetBlockHash());
    }
    m_chainman.RecalculateBestHeader();
    return true;
}

void ChainstateManager::UncheckedSourceAdded(int64_t source, bool inbound)
{
    AssertLockHeld(cs_main);
    if (source < 0) return;
    m_unchecked_sources.insert(source);
    if (inbound) m_unchecked_inbound.insert(source);
}

void ChainstateManager::AddUncheckedHolder(const CBlockIndex& index, UncheckedHeader& entry, int64_t source)
{
    AssertLockHeld(cs_main);
    if (std::ranges::find(entry.holders, source) != entry.holders.end()) return;
    entry.holders.push_back(source);
    m_unchecked_held[source].insert({index.nHeight, &index});
    if (entry.block_bytes) m_unchecked_bytes[source] += entry.block_bytes;
}

void ChainstateManager::RemoveUncheckedHolder(const CBlockIndex& index, UncheckedHeader& entry, int64_t source)
{
    AssertLockHeld(cs_main);
    const auto it{std::ranges::find(entry.holders, source)};
    if (it == entry.holders.end()) return;
    entry.holders.erase(it);
    if (auto held{m_unchecked_held.find(source)}; held != m_unchecked_held.end() && held->second.erase({index.nHeight, &index}) && held->second.empty()) m_unchecked_held.erase(held);
    if (entry.block_bytes) {
        auto bytes{m_unchecked_bytes.find(source)};
        if (Assume(bytes != m_unchecked_bytes.end() && bytes->second >= entry.block_bytes)) {
            bytes->second -= entry.block_bytes;
            if (bytes->second == 0) m_unchecked_bytes.erase(bytes);
        }
    }
}

bool ChainstateManager::HoldsUnchecked(const CBlockIndex& index, int64_t source) const
{
    AssertLockHeld(cs_main);
    const auto found{m_unchecked.find(&index)};
    return found != m_unchecked.end() && std::ranges::find(found->second.holders, source) != found->second.holders.end();
}

size_t ChainstateManager::UncheckedHeld(int64_t source) const
{
    AssertLockHeld(cs_main);
    const auto held{m_unchecked_held.find(source)};
    return held == m_unchecked_held.end() ? 0 : held->second.size();
}

size_t ChainstateManager::UncheckedBytes(int64_t source) const
{
    AssertLockHeld(cs_main);
    const auto bytes{m_unchecked_bytes.find(source)};
    return bytes == m_unchecked_bytes.end() ? 0 : bytes->second;
}

size_t ChainstateManager::UncheckedInboundHolders() const
{
    AssertLockHeld(cs_main);
    size_t holders{0};
    for (const int64_t source : m_unchecked_inbound) holders += m_unchecked_held.contains(source);
    return holders;
}

bool ChainstateManager::UncheckedExtensionFailed(const CBlockIndex& index) const
{
    AssertLockHeld(cs_main);
    const auto found{m_unchecked.find(&index)};
    return found != m_unchecked.end() && found->second.extension_failed;
}

/**
 * Whether `source` may hold `count` more unchecked headers, whose stored blocks weigh `bytes`: within its
 * own budget (an inbound peer's is smaller), and, for an inbound peer that holds none yet, if fewer than
 * inbound_holders inbound peers hold any. Nothing anyone else holds is dropped to make room.
 */
static bool FitsUncheckedBudget(const ChainstateManager& chainman, int64_t source, size_t count, size_t bytes) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    const auto limits{sidechain::Hooks().UncheckedLimits()};
    const bool inbound{chainman.m_unchecked_inbound.contains(source)};
    const size_t held{chainman.UncheckedHeld(source)};
    if (inbound && count > 0 && held == 0 && chainman.UncheckedInboundHolders() >= limits.inbound_holders) return false;
    return held + count <= (inbound ? limits.per_inbound_peer : limits.per_peer) &&
           chainman.UncheckedBytes(source) + bytes <= (inbound ? limits.block_bytes_per_inbound_peer : limits.block_bytes_per_peer);
}

bool ChainstateManager::AdmitUnchecked(const CBlockIndex& prev, int64_t source)
{
    AssertLockHeld(cs_main);
    // The node's own headers (its blocks, its disk, its operator): always.
    if (source == -1) return true;
    // A peer that is gone holds nothing (a waiting header tried again after its peer left), nor does
    // a sender not known (UNKNOWN_SOURCE): it may have been a peer.
    if (!m_unchecked_sources.contains(source)) return false;
    // The header and the unchecked headers below it the peer does not hold yet, with their blocks.
    // Within the peer's own budget, whatever other peers hold: no peer makes room by dropping what
    // another holds, so no number of peers can take or drop an honest peer's budget.
    size_t needed{1}, bytes{0};
    for (const CBlockIndex* below{&prev}; below && below->m_unchecked && !HoldsUnchecked(*below, source); below = below->pprev) {
        ++needed;
        bytes += m_unchecked.at(below).block_bytes;
    }
    return FitsUncheckedBudget(*this, source, needed, bytes);
}

void ChainstateManager::HoldUnchecked(CBlockIndex& index, int64_t source)
{
    AssertLockHeld(cs_main);
    // A new one: AcceptBlockHeader admitted it (AdmitUnchecked).
    const bool fresh{!index.m_unchecked};
    if (fresh) {
        index.m_unchecked = true;
        m_unchecked.try_emplace(&index);
    }
    if (source != -1) {
        // A peer that is gone (or a sender not known) holds nothing; a peer whose budget is spent holds no more.
        size_t needed{0}, bytes{0};
        for (const CBlockIndex* below{&index}; below && below->m_unchecked && !HoldsUnchecked(*below, source); below = below->pprev) {
            ++needed;
            bytes += m_unchecked.at(below).block_bytes;
        }
        if (needed == 0) return;
        if (!m_unchecked_sources.contains(source) || !FitsUncheckedBudget(*this, source, needed, bytes)) {
            Assume(!fresh);
            return;
        }
    }
    for (CBlockIndex* below{&index}; below && below->m_unchecked; below = below->pprev) {
        UncheckedHeader& entry{m_unchecked.at(below)};
        if (std::ranges::find(entry.holders, source) != entry.holders.end()) break;
        AddUncheckedHolder(*below, entry, source);
    }
}

bool ChainstateManager::AdmitUncheckedBlock(CBlockIndex& index, size_t bytes, int64_t source)
{
    AssertLockHeld(cs_main);
    if (!Assume(index.m_unchecked)) return true;
    // Its sender holds the header (AcceptBlockHeader made it a holder, if its budget allowed) and has
    // room for the block. The node's own: always.
    if (source != -1 && (!HoldsUnchecked(index, source) || !FitsUncheckedBudget(*this, source, 0, bytes))) return false;
    // Charged to every holder: one it does not fit stops holding the header and those on it. (An
    // honest peer holds the blocks of its own chain, which it would send itself.)
    for (const int64_t holder : std::vector<int64_t>{m_unchecked.at(&index).holders}) {
        if (holder < 0 || holder == source || FitsUncheckedBudget(*this, holder, 0, bytes)) continue;
        std::vector<const CBlockIndex*> doomed;
        for (const auto& [height, held] : std::set<std::pair<int, const CBlockIndex*>>{m_unchecked_held.at(holder)}) {
            if (height < index.nHeight || held->GetAncestor(index.nHeight) != &index) continue;
            UncheckedHeader& entry{m_unchecked.at(held)};
            RemoveUncheckedHolder(*held, entry, holder);
            if (entry.holders.empty()) doomed.push_back(held);
        }
        LogDebug(BCLog::VALIDATION, "peer=%d stops holding unchecked header %s and the %d on it: the block of %d bytes is over its budget\n", holder, index.GetBlockHash().ToString(), doomed.size(), bytes);
        // (`index` stays: its sender holds it. Every header on it that no one holds now goes.)
        if (!doomed.empty()) EraseUnchecked(std::move(doomed));
    }
    UncheckedHeader& entry{m_unchecked.at(&index)};
    Assume(entry.block_bytes == 0);
    entry.block_bytes = bytes;
    for (const int64_t holder : entry.holders) m_unchecked_bytes[holder] += bytes;
    return true;
}

void ChainstateManager::ReleaseUnchecked(CBlockIndex& index)
{
    AssertLockHeld(cs_main);
    if (!index.m_unchecked) return;
    UncheckedHeader& entry{m_unchecked.at(&index)};
    for (const int64_t holder : std::vector<int64_t>{entry.holders}) RemoveUncheckedHolder(index, entry, holder);
    m_unchecked.erase(&index);
    index.m_unchecked = false;
    m_blockman.m_dirty_blockindex.insert(&index);
    sidechain::Hooks().HeaderKept(index);
}

bool ChainstateManager::PromoteUncheckedBlock(const CBlock& block, CBlockIndex& index)
{
    AssertLockHeld(cs_main);
    const FlatFilePos old_pos{index.GetBlockPos()};
    const FlatFilePos pos{m_blockman.WriteBlock(block, index.nHeight)};
    if (pos.IsNull()) return false;
    index.nFile = pos.nFile;
    index.nDataPos = pos.nPos;
    m_blockman.m_dirty_blockindex.insert(&index);
    m_blockman.EraseVolatileBlock(old_pos);
    // No longer charged to the holders: the block is the node's from now on.
    UnchargeUncheckedBlock(index);
    return true;
}

void ChainstateManager::UnchargeUncheckedBlock(const CBlockIndex& index)
{
    AssertLockHeld(cs_main);
    const auto found{m_unchecked.find(&index)};
    if (found == m_unchecked.end() || !found->second.block_bytes) return;
    for (const int64_t holder : found->second.holders) {
        auto bytes{m_unchecked_bytes.find(holder)};
        if (!Assume(bytes != m_unchecked_bytes.end() && bytes->second >= found->second.block_bytes)) continue;
        bytes->second -= found->second.block_bytes;
        if (bytes->second == 0) m_unchecked_bytes.erase(bytes);
    }
    found->second.block_bytes = 0;
}

void ChainstateManager::UncheckedSourceGone(int64_t source)
{
    AssertLockHeld(cs_main);
    m_unchecked_sources.erase(source);
    m_unchecked_inbound.erase(source);
    const auto held{m_unchecked_held.find(source)};
    if (held == m_unchecked_held.end()) return;
    std::vector<const CBlockIndex*> doomed;
    for (const auto& [_, index] : std::set<std::pair<int, const CBlockIndex*>>{held->second}) {
        UncheckedHeader& entry{m_unchecked.at(index)};
        RemoveUncheckedHolder(*index, entry, source);
        if (entry.holders.empty()) doomed.push_back(index);
    }
    // (Every header on top of one no one holds is held by no one either.)
    if (!doomed.empty()) {
        LogDebug(BCLog::VALIDATION, "%d unchecked headers no peer holds since peer=%d left: taken out of the block index\n", doomed.size(), source);
        EraseUnchecked(std::move(doomed));
    }
}

void ChainstateManager::EraseUnchecked(std::vector<const CBlockIndex*> doomed)
{
    AssertLockHeld(cs_main);
    std::unordered_set<const CBlockIndex*> erased{doomed.begin(), doomed.end()};
    // Where each one's pointers go: the nearest block below it that stays.
    std::vector<std::pair<const CBlockIndex*, const CBlockIndex*>> moved;
    moved.reserve(doomed.size());
    for (const CBlockIndex* index : doomed) {
        Assume(index->m_unchecked && !index->IsValid(BLOCK_VALID_SCRIPTS));
        const CBlockIndex* stays{index->pprev};
        while (stays && erased.contains(stays)) stays = stays->pprev;
        moved.emplace_back(index, stays);
    }
    // Code that keeps a pointer into the block index across a release of cs_main looks it up again.
    ++m_unchecked_erasures;
    // Whoever keeps pointers into the block index moves them first (they may read the entries).
    if (m_on_unchecked_erased) m_on_unchecked_erased(moved);
    const bool best_header_erased{erased.contains(m_best_header)};
    for (auto it{m_blockman.m_blocks_unlinked.begin()}; it != m_blockman.m_blocks_unlinked.end();) {
        if (erased.contains(it->first) || erased.contains(it->second)) {
            it = m_blockman.m_blocks_unlinked.erase(it);
        } else {
            ++it;
        }
    }
    // The entries of the batches before go first (beyond the bound): this one is kept whole.
    while (m_erased_headers.size() > MAX_ERASED_HEADERS) m_erased_headers.pop_front();
    // Highest first: no entry is erased before those on top of it.
    std::ranges::sort(doomed, [](const CBlockIndex* a, const CBlockIndex* b) { return a->nHeight > b->nHeight; });
    for (const CBlockIndex* index : doomed) {
        CBlockIndex* entry{const_cast<CBlockIndex*>(index)};
        for (const auto& chainstate : m_chainstates) chainstate->setBlockIndexCandidates.erase(entry);
        m_blockman.m_dirty_blockindex.erase(entry);
        if (m_best_invalid == index) m_best_invalid = nullptr;
        if (m_last_notified_header == index) m_last_notified_header = nullptr;
        if (m_mainchain) m_mainchain->ForgetFailure(index->GetBlockHash());
        if (auto found{m_unchecked.find(index)}; found != m_unchecked.end()) {
            for (const int64_t holder : std::vector<int64_t>{found->second.holders}) RemoveUncheckedHolder(*index, found->second, holder);
            m_unchecked.erase(found);
        }
        // Its block, stored apart (never in the block files), is deleted with it.
        if (entry->nStatus & BLOCK_HAVE_DATA) {
            const FlatFilePos pos{entry->GetBlockPos()};
            if (node::BlockManager::IsVolatile(pos)) m_blockman.EraseVolatileBlock(pos);
            entry->nStatus &= ~BLOCK_HAVE_DATA;
        }
        m_erased_headers.push_back(m_blockman.m_block_index.extract(index->GetBlockHash()));
        if (m_test_poison_erased) {
            CBlockIndex& dead{m_erased_headers.back().mapped()};
            dead.nHeight = -1;
            dead.nTime = 0;
            dead.nStatus = BLOCK_FAILED_VALID;
            dead.nChainWork = 0;
            dead.pprev = nullptr;
            dead.pskip = nullptr;
        }
    }
    if (best_header_erased) RecalculateBestHeader();
}

void ChainstateManager::CheckUnchecked() const
{
    AssertLockHeld(cs_main);
    size_t flagged{0};
    std::map<int64_t, size_t> bytes;
    for (const auto& [_, index] : m_blockman.m_block_index) {
        // Only unchecked headers have their blocks stored apart.
        if (index.nStatus & BLOCK_HAVE_DATA) assert(index.m_unchecked || !node::BlockManager::IsVolatile(index.GetBlockPos()));
        if (!index.m_unchecked) {
            assert(!m_unchecked.contains(&index));
            continue;
        }
        ++flagged;
        const auto found{m_unchecked.find(&index)};
        assert(found != m_unchecked.end());
        // Unchecked headers are not connected, someone holds each, and their holders hold the
        // unchecked headers below them.
        assert(!ActiveChain().Contains(index));
        const auto& holders{found->second.holders};
        assert(!holders.empty());
        if (index.pprev && index.pprev->m_unchecked) {
            const auto& below{m_unchecked.at(index.pprev).holders};
            for (const int64_t holder : holders) assert(std::ranges::find(below, holder) != below.end());
        }
        // A stored block not yet connected is stored apart, and charged to the header's holders.
        const size_t block_bytes{found->second.block_bytes};
        assert((block_bytes > 0) == ((index.nStatus & BLOCK_HAVE_DATA) && node::BlockManager::IsVolatile(index.GetBlockPos())));
        const std::pair<int, const CBlockIndex*> key{index.nHeight, &index};
        for (const int64_t holder : holders) {
            assert(m_unchecked_held.at(holder).contains(key));
            if (block_bytes) bytes[holder] += block_bytes;
        }
    }
    assert(flagged == m_unchecked.size());
    size_t pairs{0}, held{0};
    for (const auto& [_, entry] : m_unchecked) pairs += entry.holders.size();
    for (const auto& [_, set] : m_unchecked_held) held += set.size();
    assert(pairs == held);
    assert(bytes == m_unchecked_bytes);
    // Inbound peers are connected peers; every holder but the node is one.
    for (const int64_t source : m_unchecked_inbound) assert(m_unchecked_sources.contains(source));
    for (const auto& [holder, _] : m_unchecked_held) assert(holder < 0 || m_unchecked_sources.contains(holder));
}

void Chainstate::ReplaceBlockExtension(CBlockIndex* pindex, const BlockHeaderExtension& ext)
{
    AssertLockHeld(cs_main);
    LogInfo("Block %s: its header's extension is replaced by another (the one the node had is not valid)", pindex->GetBlockHash().ToString());
    pindex->m_ext = std::make_shared<const BlockHeaderExtension>(ext);
    m_blockman.m_dirty_blockindex.insert(pindex);
    if (const auto found{m_chainman.m_unchecked.find(pindex)}; found != m_chainman.m_unchecked.end()) found->second.extension_failed = false;
    // (Whoever took the headers in may want to connect what it gave back: PeerManager.)
    ++m_chainman.m_extensions_replaced;
    // What is stored is written again with it: the node serves its blocks as they are on disk.
    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        CBlock block;
        if (m_blockman.ReadBlock(block, *pindex)) {
            // A block stored apart (its header unchecked) is written again in its own file, in place.
            const FlatFilePos old_pos{pindex->GetBlockPos()};
            const FlatFilePos pos{node::BlockManager::IsVolatile(old_pos) ? m_blockman.WriteVolatileBlock(block, old_pos) : m_blockman.WriteBlock(block, pindex->nHeight)};
            if (!pos.IsNull()) {
                pindex->nFile = pos.nFile;
                pindex->nDataPos = pos.nPos;
                // (Its undo data, if any, was in the undo file of the old block file: written again when connected.)
                pindex->nUndoPos = 0;
                pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            }
        }
    }
    if (!(pindex->nStatus & BLOCK_FAILED_VALID)) return;
    if (pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_VALID)) return;
    sidechain::Mainchain* mainchain{m_chainman.m_mainchain.get()};
    if (mainchain && mainchain->GetFailure(pindex->GetBlockHash()) == sidechain::Mainchain::Failure::MANUAL) return;
    std::vector<CBlockIndex*> failed;
    for (auto& [_, block_index] : m_blockman.m_block_index) {
        if ((block_index.nStatus & BLOCK_FAILED_VALID) && block_index.GetAncestor(pindex->nHeight) == pindex) failed.push_back(&block_index);
    }
    std::sort(failed.begin(), failed.end(), [](const CBlockIndex* a, const CBlockIndex* b) { return a->nHeight < b->nHeight; });
    for (CBlockIndex* block_index : failed) {
        if (block_index != pindex && (block_index->pprev->nStatus & BLOCK_FAILED_VALID)) continue;
        if (block_index != pindex && mainchain && mainchain->GetFailure(block_index->GetBlockHash()) == sidechain::Mainchain::Failure::MANUAL) continue;
        block_index->nStatus &= ~BLOCK_FAILED_VALID;
        m_blockman.m_dirty_blockindex.insert(block_index);
        if (block_index->IsValid(BLOCK_VALID_TRANSACTIONS) && block_index->HaveNumChainTxs() && setBlockIndexCandidates.value_comp()(m_chain.Tip(), block_index)) {
            setBlockIndexCandidates.insert(block_index);
        }
        if (block_index == m_chainman.m_best_invalid) m_chainman.m_best_invalid = nullptr;
        if (mainchain) mainchain->ForgetFailure(block_index->GetBlockHash());
    }
    m_chainman.RecalculateBestHeader();
}

void Chainstate::TryAddBlockIndexCandidate(CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    // Do not continue building a chainstate that is based on an invalid
    // snapshot. This is a belt-and-suspenders type of check because if an
    // invalid snapshot is loaded, the node will shut down to force a manual
    // intervention. But it is good to handle this case correctly regardless.
    if (m_assumeutxo == Assumeutxo::INVALID) {
        return;
    }

    // The block only is a candidate for the most-work-chain if it has the same
    // or more work than our current tip.
    if (m_chain.Tip() != nullptr && setBlockIndexCandidates.value_comp()(pindex, m_chain.Tip())) {
        return;
    }

    const CBlockIndex* target_block{TargetBlock()};
    if (!target_block) {
        // If no specific target block, add all entries that have more
        // work than the tip.
        setBlockIndexCandidates.insert(pindex);
    } else {
        // If there is a target block, only consider connecting blocks
        // towards the target block.
        if (target_block->GetAncestor(pindex->nHeight) == pindex) {
            setBlockIndexCandidates.insert(pindex);
        }
    }
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
void ChainstateManager::ReceivedBlockTransactions(const CBlock& block, CBlockIndex* pindexNew, const FlatFilePos& pos)
{
    AssertLockHeld(cs_main);
    pindexNew->nTx = block.vtx.size();
    // Typically m_chain_tx_count will be 0 at this point, but it can be nonzero if this
    // is a pruned block which is being downloaded again, or if this is an
    // assumeutxo snapshot block which has a hardcoded m_chain_tx_count value from the
    // snapshot metadata. If the pindex is not the snapshot block and the
    // m_chain_tx_count value is not zero, assert that value is actually correct.
    auto prev_tx_sum = [](CBlockIndex& block) { return block.nTx + (block.pprev ? block.pprev->m_chain_tx_count : 0); };
    if (!Assume(pindexNew->m_chain_tx_count == 0 || pindexNew->m_chain_tx_count == prev_tx_sum(*pindexNew) ||
                std::ranges::any_of(m_chainstates, [&](const auto& cs) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return cs->SnapshotBase() == pindexNew; }))) {
        LogWarning("Internal bug detected: block %d has unexpected m_chain_tx_count %i that should be %i (%s %s). Please report this issue here: %s\n",
            pindexNew->nHeight, pindexNew->m_chain_tx_count, prev_tx_sum(*pindexNew), CLIENT_NAME, FormatFullVersion(), CLIENT_BUGREPORT);
        pindexNew->m_chain_tx_count = 0;
    }
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    if (DeploymentActiveAt(*pindexNew, *this, Consensus::DEPLOYMENT_SEGWIT)) {
        pindexNew->nStatus |= BLOCK_OPT_WITNESS;
    }
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    m_blockman.m_dirty_blockindex.insert(pindexNew);

    if (pindexNew->pprev == nullptr || pindexNew->pprev->HaveNumChainTxs()) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            // Before setting m_chain_tx_count, assert that it is 0 or already set to
            // the correct value. This assert will fail after receiving the
            // assumeutxo snapshot block if assumeutxo snapshot metadata has an
            // incorrect hardcoded AssumeutxoData::m_chain_tx_count value.
            if (!Assume(pindex->m_chain_tx_count == 0 || pindex->m_chain_tx_count == prev_tx_sum(*pindex))) {
                LogWarning("Internal bug detected: block %d has unexpected m_chain_tx_count %i that should be %i (%s %s). Please report this issue here: %s\n",
                   pindex->nHeight, pindex->m_chain_tx_count, prev_tx_sum(*pindex), CLIENT_NAME, FormatFullVersion(), CLIENT_BUGREPORT);
            }
            pindex->m_chain_tx_count = prev_tx_sum(*pindex);
            pindex->nSequenceId = nBlockSequenceId++;
            for (const auto& c : m_chainstates) {
                c->TryAddBlockIndexCandidate(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = m_blockman.m_blocks_unlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                m_blockman.m_blocks_unlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            m_blockman.AddUnlinkedBlock(pindexNew);
        }
    }
}

static bool CheckBlockHeader(const CBlockHeader& block, BlockValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW = true)
{
    // Check proof of work matches claimed amount (none for a block of a kind of a sidechain's own)
    if (fCheckPOW && !sidechain::CheckHeaderWork(block, consensusParams))
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "high-hash", "proof of work failed");

    return true;
}

static bool CheckMerkleRoot(const CBlock& block, BlockValidationState& state)
{
    if (block.m_checked_merkle_root) return true;

    bool mutated;
    uint256 merkle_root = BlockMerkleRoot(block, &mutated);
    if (block.hashMerkleRoot != merkle_root) {
        return state.Invalid(
            /*result=*/BlockValidationResult::BLOCK_MUTATED,
            /*reject_reason=*/"bad-txnmrklroot",
            /*debug_message=*/"hashMerkleRoot mismatch");
    }

    // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
    // of transactions in a block without affecting the merkle root of a block,
    // while still invalidating it.
    if (mutated) {
        return state.Invalid(
            /*result=*/BlockValidationResult::BLOCK_MUTATED,
            /*reject_reason=*/"bad-txns-duplicate",
            /*debug_message=*/"duplicate transaction");
    }

    block.m_checked_merkle_root = true;
    return true;
}

/** CheckWitnessMalleation performs checks for block malleation with regard to
 * its witnesses.
 *
 * Note: If the witness commitment is expected (i.e. `expect_witness_commitment
 * = true`), then the block is required to have at least one transaction and the
 * first transaction needs to have at least one input. */
static bool CheckWitnessMalleation(const CBlock& block, bool expect_witness_commitment, BlockValidationState& state)
{
    if (expect_witness_commitment) {
        if (block.m_checked_witness_commitment) return true;

        int commitpos = GetWitnessCommitmentIndex(block);
        if (commitpos != NO_WITNESS_COMMITMENT) {
            assert(!block.vtx.empty() && !block.vtx[0]->vin.empty());
            const auto& witness_stack{block.vtx[0]->vin[0].scriptWitness.stack};

            if (witness_stack.size() != 1 || witness_stack[0].size() != 32) {
                return state.Invalid(
                    /*result=*/BlockValidationResult::BLOCK_MUTATED,
                    /*reject_reason=*/"bad-witness-nonce-size",
                    /*debug_message=*/strprintf("%s : invalid witness reserved value size", __func__));
            }

            // The malleation check is ignored; as the transaction tree itself
            // already does not permit it, it is impossible to trigger in the
            // witness tree.
            uint256 hash_witness = BlockWitnessMerkleRoot(block);

            CHash256().Write(hash_witness).Write(witness_stack[0]).Finalize(hash_witness);
            if (memcmp(hash_witness.begin(), &block.vtx[0]->vout[commitpos].scriptPubKey[6], 32)) {
                return state.Invalid(
                    /*result=*/BlockValidationResult::BLOCK_MUTATED,
                    /*reject_reason=*/"bad-witness-merkle-match",
                    /*debug_message=*/strprintf("%s : witness merkle commitment mismatch", __func__));
            }

            block.m_checked_witness_commitment = true;
            return true;
        }
    }

    // No witness data is allowed in blocks that don't commit to witness data, as this would otherwise leave room for spam
    for (const auto& tx : block.vtx) {
        if (tx->HasWitness()) {
            return state.Invalid(
                /*result=*/BlockValidationResult::BLOCK_MUTATED,
                /*reject_reason=*/"unexpected-witness",
                /*debug_message=*/strprintf("%s : unexpected witness data found", __func__));
        }
    }

    return true;
}

bool CheckBlock(const CBlock& block, BlockValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW, bool fCheckMerkleRoot)
{
    // These are checks that are independent of context.

    if (block.fChecked)
        return true;

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, consensusParams, fCheckPOW))
        return false;

    // Signet only: check block solution
    if (consensusParams.signet_blocks && fCheckPOW && !CheckSignetBlockSolution(block, consensusParams)) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-signet-blksig", "signet block signature validation failure");
    }

    // Check the merkle root.
    if (fCheckMerkleRoot && !CheckMerkleRoot(block, state)) {
        return false;
    }
    // And, in an extended block, the sections: like the transactions, data the header commits to.
    if (fCheckMerkleRoot && !sidechain::CheckSections(block)) {
        return state.Invalid(BlockValidationResult::BLOCK_MUTATED, "bad-sections-hash", "the sections are not those the header commits to");
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.
    // Note that witness malleability is checked in ContextualCheckBlock, so no
    // checks that use witness data may be performed here.

    // Size limits
    if (block.vtx.empty() || block.vtx.size() * WITNESS_SCALE_FACTOR > consensusParams.max_block_weight || ::GetSerializeSize(TX_NO_WITNESS(block)) * WITNESS_SCALE_FACTOR > consensusParams.max_block_weight)
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-length", "size limits failed");

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase())
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-cb-missing", "first tx is not coinbase");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i]->IsCoinBase())
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-cb-multiple", "more than one coinbase");

    // Check transactions
    // Must check for duplicate inputs (see CVE-2018-17144)
    for (const auto& tx : block.vtx) {
        TxValidationState tx_state;
        if (!CheckTransaction(*tx, tx_state)) {
            // CheckBlock() does context-free validation checks. The only
            // possible failures are consensus failures.
            assert(tx_state.GetResult() == TxValidationResult::TX_CONSENSUS);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, tx_state.GetRejectReason(),
                                 strprintf("Transaction check failed (tx hash %s) %s", tx->GetHash().ToString(), tx_state.GetDebugMessage()));
        }
    }
    // Refuse any duplicate transaction. Upstream relies on a duplicate double-spending its own
    // inputs, but a sidechain may allow transactions without inputs, and the merkle "mutated"
    // check only catches duplicates that end up as adjacent pairs in the tree.
    {
        std::unordered_set<Txid, SaltedTxidHasher> txids;
        txids.reserve(block.vtx.size());
        for (const auto& tx : block.vtx) {
            if (!txids.insert(tx->GetHash()).second) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-duplicate",
                                     strprintf("duplicate transaction %s", tx->GetHash().ToString()));
            }
        }
    }

    // This underestimates the number of sigops, because unlike ConnectBlock it
    // does not count witness and p2sh sigops.
    unsigned int nSigOps = 0;
    for (const auto& tx : block.vtx)
    {
        nSigOps += GetLegacySigOpCount(*tx);
    }
    // Without the height here: the limit of any height. ConnectBlock applies the one of the block's.
    if (nSigOps * WITNESS_SCALE_FACTOR > consensusParams.MaxBlockSigOpsCost())
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-sigops", "out-of-bounds SigOpCount");

    // The context free rules of a sidechain's own (none in the template).
    if (!sidechain::Hooks().CheckBlock(block, state)) return false;

    if (fCheckPOW && fCheckMerkleRoot)
        block.fChecked = true;

    return true;
}

void ChainstateManager::UpdateUncommittedBlockStructures(CBlock& block, const CBlockIndex* pindexPrev) const
{
    int commitpos = GetWitnessCommitmentIndex(block);
    static const std::vector<unsigned char> nonce(32, 0x00);
    if (commitpos != NO_WITNESS_COMMITMENT && DeploymentActiveAfter(pindexPrev, *this, Consensus::DEPLOYMENT_SEGWIT) && !block.vtx[0]->HasWitness()) {
        CMutableTransaction tx(*block.vtx[0]);
        tx.vin[0].scriptWitness.stack.resize(1);
        tx.vin[0].scriptWitness.stack[0] = nonce;
        block.vtx[0] = MakeTransactionRef(std::move(tx));
    }
}

void ChainstateManager::GenerateCoinbaseCommitment(CBlock& block, const CBlockIndex* pindexPrev) const
{
    int commitpos = GetWitnessCommitmentIndex(block);
    std::vector<unsigned char> ret(32, 0x00);
    if (commitpos == NO_WITNESS_COMMITMENT) {
        uint256 witnessroot = BlockWitnessMerkleRoot(block);
        CHash256().Write(witnessroot).Write(ret).Finalize(witnessroot);
        CTxOut out;
        out.nValue = 0;
        out.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT);
        out.scriptPubKey[0] = OP_RETURN;
        out.scriptPubKey[1] = 0x24;
        out.scriptPubKey[2] = 0xaa;
        out.scriptPubKey[3] = 0x21;
        out.scriptPubKey[4] = 0xa9;
        out.scriptPubKey[5] = 0xed;
        memcpy(&out.scriptPubKey[6], witnessroot.begin(), 32);
        CMutableTransaction tx(*block.vtx[0]);
        tx.vout.push_back(out);
        block.vtx[0] = MakeTransactionRef(std::move(tx));
    }
    UpdateUncommittedBlockStructures(block, pindexPrev);
}

bool HasValidProofOfWork(std::span<const CBlockHeader> headers, const Consensus::Params& consensusParams)
{
    return std::ranges::all_of(headers,
                               [&](const auto& header) { return sidechain::CheckHeaderWork(header, consensusParams); });
}

bool IsBlockMutated(const CBlock& block, bool check_witness_root)
{
    BlockValidationState state;
    if (!CheckMerkleRoot(block, state)) {
        LogDebug(BCLog::VALIDATION, "Block mutated: %s\n", state.ToString());
        return true;
    }
    if (!sidechain::CheckSections(block)) {
        LogDebug(BCLog::VALIDATION, "Block mutated: its sections are not those its header commits to\n");
        return true;
    }

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        // Consider the block mutated if any transaction is 64 bytes in size (see 3.1
        // in "Weaknesses in Bitcoin’s Merkle Root Construction":
        // https://lists.linuxfoundation.org/pipermail/bitcoin-dev/attachments/20190225/a27d8837/attachment-0001.pdf).
        //
        // Note: This is not a consensus change as this only applies to blocks that
        // don't have a coinbase transaction and would therefore already be invalid.
        return std::any_of(block.vtx.begin(), block.vtx.end(),
                           [](auto& tx) { return GetSerializeSize(TX_NO_WITNESS(tx)) == 64; });
    } else {
        // Theoretically it is still possible for a block with a 64 byte
        // coinbase transaction to be mutated but we neglect that possibility
        // here as it requires at least 224 bits of work.
    }

    if (!CheckWitnessMalleation(block, check_witness_root, state)) {
        LogDebug(BCLog::VALIDATION, "Block mutated: %s\n", state.ToString());
        return true;
    }

    return false;
}

arith_uint256 CalculateClaimedHeadersWork(std::span<const CBlockHeader> headers)
{
    arith_uint256 total_work{0};
    for (const CBlockHeader& header : headers) {
        total_work += GetBlockProof(header);
    }
    return total_work;
}

/** Context-dependent validity checks.
 *  By "context", we mean only the previous block headers, but not the UTXO
 *  set; UTXO-related validity checks are done in ConnectBlock().
 *  NOTE: This function is not currently invoked by ConnectBlock(), so we
 *  should consider upgrade issues if we change which consensus rules are
 *  enforced in this function (eg by adding a new consensus rule). See comment
 *  in ConnectBlock().
 *  Note that -reindex-chainstate skips the validation that happens here!
 *
 *  NOTE: failing to check the header's height against the last checkpoint's opened a DoS vector between
 *  v0.12 and v0.15 (when no additional protection was in place) whereby an attacker could unboundedly
 *  grow our in-memory block index. See https://bitcoincore.org/en/2024/07/03/disclose-header-spam.
 */
static bool ContextualCheckBlockHeader(const CBlockHeader& block, BlockValidationState& state, const ChainstateManager& chainman, const CBlockIndex* pindexPrev) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);
    assert(pindexPrev != nullptr);
    const int nHeight = pindexPrev->nHeight + 1;

    // A block of a kind of a sidechain's own has no proof of work; the sidechain checks its header.
    const Consensus::Params& consensusParams = chainman.GetConsensus();
    const sidechain::BlockKind kind{sidechain::Hooks().Kind(block)};

    // Check proof of work
    if (kind == sidechain::BlockKind::BMM && block.nBits != GetNextWorkRequired(pindexPrev, &block, consensusParams))
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "bad-diffbits", "incorrect proof of work");

    // Check timestamp against prev
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast())
        return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "time-too-old", "block's timestamp is too early");

    if (kind != sidechain::BlockKind::BMM) {
        if (!sidechain::Hooks().CheckHeader(block, *pindexPrev, chainman, state)) {
            if (state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE && sidechain::Hooks().IsWaitReason(state.GetRejectReason())) {
                chainman.AddBmmWaiting(block, /*peer=*/-1);
            }
            return false;
        }
    } else if (consensusParams.sidechain.enabled) {
        // Blind merged mining: a block of a sidechain is one that a mainchain block
        // committed to, and the mainchain commits to the blocks of a chain in order.
        const sidechain::Mainchain& mainchain{*Assert(chainman.m_mainchain)};
        const uint256 hash{block.GetHash()};
        auto bmm_height{mainchain.BmmHeight(hash)};
        if (!bmm_height && chainman.m_mainchain_poll) {
            // The commitment may be in a mainchain block that is not on record yet.
            chainman.m_mainchain_poll();
            bmm_height = mainchain.BmmHeight(hash);
        }
        // Not a reason to remember the block as invalid: the commitment may still come.
        if (!bmm_height) {
            // Bounded, as anyone can send headers that will never be committed to.
            chainman.AddBmmWaiting(block, /*peer=*/-1);
            return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "bmm-unknown", "no mainchain block is known to commit to this block");
        }
        // In order after the last block below that needs a commitment: the parent, in the template.
        const CBlockIndex* committed_prev{pindexPrev};
        std::optional<uint256> prev_commitment{sidechain::Hooks().CommitmentOf(*committed_prev)};
        while (!prev_commitment && committed_prev->pprev) {
            committed_prev = committed_prev->pprev;
            prev_commitment = sidechain::Hooks().CommitmentOf(*committed_prev);
        }
        if (committed_prev->pprev && prev_commitment) {
            const auto prev_bmm_height{mainchain.BmmHeight(*prev_commitment)};
            if (!prev_bmm_height) return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "bmm-prev-unknown", "no mainchain block is known to commit to the previous block");
            if (*bmm_height <= *prev_bmm_height) return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "bmm-order", "the mainchain committed to this block before it committed to the previous block");
        }
    }

    // Testnet4 and regtest only: Check timestamp against prev for difficulty-adjustment
    // blocks to prevent timewarp attacks (see https://github.com/bitcoin/bitcoin/pull/15482).
    if (consensusParams.enforce_BIP94) {
        // Check timestamp for the first block of each difficulty adjustment
        // interval, except the genesis block.
        if (nHeight % consensusParams.DifficultyAdjustmentInterval() == 0) {
            if (block.GetBlockTime() < pindexPrev->GetBlockTime() - MAX_TIMEWARP) {
                return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "time-timewarp-attack", "block's timestamp is too early on diff adjustment block");
            }
        }
    }

    // Check timestamp (a sidechain may allow less than Bitcoin's two hours)
    if (block.Time() > NodeClock::now() + std::chrono::seconds{sidechain::Hooks().MaxFutureBlockTime()}) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "time-too-new", "block timestamp too far in the future");
    }

    // Reject blocks with outdated version
    if ((block.nVersion < 2 && DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_HEIGHTINCB)) ||
        (block.nVersion < 3 && DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_DERSIG)) ||
        (block.nVersion < 4 && DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_CLTV))) {
            return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, strprintf("bad-version(0x%08x)", block.nVersion),
                                 strprintf("rejected nVersion=0x%08x block", block.nVersion));
    }

    return true;
}

/** NOTE: This function is not currently invoked by ConnectBlock(), so we
 *  should consider upgrade issues if we change which consensus rules are
 *  enforced in this function (eg by adding a new consensus rule). See comment
 *  in ConnectBlock().
 *  Note that -reindex-chainstate skips the validation that happens here!
 */
static bool ContextualCheckBlock(const CBlock& block, BlockValidationState& state, const ChainstateManager& chainman, const CBlockIndex* pindexPrev)
{
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Enforce BIP113 (Median Time Past).
    bool enforce_locktime_median_time_past{false};
    if (DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_CSV)) {
        assert(pindexPrev != nullptr);
        enforce_locktime_median_time_past = true;
    }

    const int64_t nLockTimeCutoff{enforce_locktime_median_time_past ?
                                      pindexPrev->GetMedianTimePast() :
                                      block.GetBlockTime()};

    // Check that all transactions are finalized
    for (const auto& tx : block.vtx) {
        if (!IsFinalTx(*tx, nHeight, nLockTimeCutoff)) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-txns-nonfinal", "non-final transaction");
        }
    }

    // Enforce rule that the coinbase starts with serialized block height
    if (DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_HEIGHTINCB))
    {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(), block.vtx[0]->vin[0].scriptSig.begin())) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-cb-height", "block height mismatch in coinbase");
        }
    }

    // Validation for witness commitments.
    // * We compute the witness hash (which is the hash including witnesses) of all the block's transactions, except the
    //   coinbase (where 0x0000....0000 is used instead).
    // * The coinbase scriptWitness is a stack of a single 32-byte vector, containing a witness reserved value (unconstrained).
    // * We build a merkle tree with all those witness hashes as leaves (similar to the hashMerkleRoot in the block header).
    // * There must be at least one output whose scriptPubKey is a single 36-byte push, the first 4 bytes of which are
    //   {0xaa, 0x21, 0xa9, 0xed}, and the following 32 bytes are SHA256^2(witness root, witness reserved value). In case there are
    //   multiple, the last one is used.
    if (!CheckWitnessMalleation(block, DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_SEGWIT), state)) {
        return false;
    }

    // After the coinbase witness reserved value and commitment are verified,
    // we can check if the block weight passes (before we've checked the
    // coinbase witness, it would be possible for the weight to be too
    // large by filling up the coinbase witness, which doesn't change
    // the block hash, so we couldn't mark the block as permanently
    // failed).
    const Consensus::Params& consensus_params{chainman.GetConsensus()};
    if (GetBlockWeight(block) > consensus_params.max_block_weight) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-weight", strprintf("%s : weight limit failed", __func__));
    }
    // The part of the block set aside for the coinbase cannot be used by other transactions.
    if (consensus_params.max_block_tx_weight < consensus_params.max_block_weight) {
        int64_t tx_weight{0};
        for (size_t i{1}; i < block.vtx.size(); ++i) tx_weight += GetTransactionWeight(*block.vtx[i]);
        if (tx_weight > consensus_params.max_block_tx_weight) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-blk-tx-weight", strprintf("%s : transaction weight limit failed", __func__));
        }
    }

    return true;
}

/**
 * Why a block marked failed, or a header on it, is no fault of the peer that sends it, if it is not
 * (nullptr otherwise): what the lowest failed block it descends from (or it) failed for, as for
 * ReconsiderRecordFailure. Against the record of the mainchain ("record-failed", where the hooks
 * pardon such failures: the peer's record may be ahead), not the first noted failure below it (a block
 * on one the operator marked invalid can be noted as failed against the record too, and the record
 * does not take back what the operator did); or for the extension of a header the node took unchecked
 * ("extension-failed": the copy's fault, ChainHooks::ExtensionFailure).
 */
static const char* PardonedFailure(const ChainstateManager& chainman, const CBlockIndex* index) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (!index || !(index->nStatus & BLOCK_FAILED_VALID)) return nullptr;
    const CBlockIndex* root{index};
    while (root->pprev && (root->pprev->nStatus & BLOCK_FAILED_VALID)) root = root->pprev;
    if (chainman.UncheckedExtensionFailed(*root)) return "extension-failed";
    if (chainman.m_mainchain && sidechain::Hooks().PardonRecordFailures() && chainman.m_mainchain->GetFailure(root->GetBlockHash()) == sidechain::Mainchain::Failure::RECORD) return "record-failed";
    return nullptr;
}

bool ChainstateManager::SpareWanted(const CBlockHeader& header, const CBlockIndex& prev, int64_t source) const
{
    AssertLockHeld(cs_main);
    const arith_uint256 work{sidechain::Hooks().CustomChainOrder() ? sidechain::Hooks().ChainWork(&prev, header) : prev.nChainWork + GetBlockProof(header)};
    // The most work (or, with the chain's own order, the best key: more anchors, say): the best header.
    if (m_best_header && work > m_best_header->nChainWork) return true;
    // An announcement from an outbound peer: a tip better than the active chain's.
    const bool outbound{m_unchecked_sources.contains(source) && !m_unchecked_inbound.contains(source)};
    const CBlockIndex* tip{ActiveChain().Tip()};
    return outbound && tip && work > tip->nChainWork;
}

bool ChainstateManager::AcceptBlockHeader(const CBlockHeader& block, BlockValidationState& state, CBlockIndex** ppindex, bool min_pow_checked, int64_t source, bool continued)
{
    AssertLockHeld(cs_main);

    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf{m_blockman.m_block_index.find(hash)};
    // A header the node cannot check all of yet, or one on top of such a header: unchecked, taken
    // within the budget of the peer that sends it (ChainHooks::HeaderCheckable, m_unchecked).
    bool unchecked{false};
    if (hash != GetConsensus().hashGenesisBlock) {
        if (miSelf != m_blockman.m_block_index.end()) {
            // Block header is already known.
            CBlockIndex* pindex = &(miSelf->second);
            if (ppindex)
                *ppindex = pindex;
            // With another extension: valid where the one the node has is not (a signature forged
            // before the node could check it), or any other if the block failed with the one it has
            // (the copy's fault: an unchecked header's extension failure), the node takes this one,
            // and gives the block another chance if it failed with the old one. (Not for a block of
            // the active chain: its extension was checked when it was connected.)
            if (pindex->m_ext && !sidechain::SameExtension(block, *pindex) && !ActiveChain().Contains(*pindex)) {
                const sidechain::OtherExtension other{sidechain::Hooks().CheckOtherExtension(block, *pindex)};
                if (other == sidechain::OtherExtension::REPLACE || (other == sidechain::OtherExtension::UNKNOWN && UncheckedExtensionFailed(*pindex))) {
                    ActiveChainstate().ReplaceBlockExtension(pindex, block.ext);
                }
            }
            // An unchecked header: the peer that sends it holds it too.
            if (pindex->m_unchecked) HoldUnchecked(*pindex, source);
            if (pindex->nStatus & BLOCK_FAILED_VALID) {
                LogDebug(BCLog::VALIDATION, "%s: block %s is marked invalid\n", __func__, hash.ToString());
                // Against the record, which the peer's may be ahead of, or the copy's fault: no fault of the peer's.
                if (const char* pardoned{PardonedFailure(*this, pindex)}) {
                    return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, pardoned, strprintf("block %s failed through no fault of the peer's", hash.ToString()));
                }
                return state.Invalid(BlockValidationResult::BLOCK_CACHED_INVALID, "duplicate-invalid",
                                     strprintf("block %s was previously marked invalid", hash.ToString()));
            }
            return true;
        }

        if (!CheckBlockHeader(block, state, GetConsensus())) {
            LogDebug(BCLog::VALIDATION, "%s: Consensus::CheckBlockHeader: %s, %s\n", __func__, hash.ToString(), state.ToString());
            return false;
        }

        // Get prev block index
        CBlockIndex* pindexPrev = nullptr;
        BlockMap::iterator mi{m_blockman.m_block_index.find(block.hashPrevBlock)};
        if (mi == m_blockman.m_block_index.end()) {
            LogDebug(BCLog::VALIDATION, "header %s has prev block not found: %s\n", hash.ToString(), block.hashPrevBlock.ToString());
            return state.Invalid(BlockValidationResult::BLOCK_MISSING_PREV, "prev-blk-not-found");
        }
        pindexPrev = &((*mi).second);
        if (pindexPrev->nStatus & BLOCK_FAILED_VALID) {
            LogDebug(BCLog::VALIDATION, "header %s has prev block invalid: %s\n", hash.ToString(), block.hashPrevBlock.ToString());
            if (const char* pardoned{PardonedFailure(*this, pindexPrev)}) {
                return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, pardoned, strprintf("block %s failed through no fault of the peer's", block.hashPrevBlock.ToString()));
            }
            return state.Invalid(BlockValidationResult::BLOCK_INVALID_PREV, "bad-prevblk");
        }
        unchecked = pindexPrev->m_unchecked || !sidechain::Hooks().HeaderCheckable(block, *pindexPrev);
        // One of a kind the block index has as many of as the chain can need (a signature that costs
        // nothing, made again and again): from a peer, not kept for good (ChainHooks::HeaderSpare).
        const bool spare{!unchecked && source != -1 && sidechain::Hooks().HeaderSpare(block, *pindexPrev)};
        if (!ContextualCheckBlockHeader(block, state, *this, pindexPrev)) {
            LogDebug(BCLog::VALIDATION, "%s: Consensus::ContextualCheckBlockHeader: %s, %s\n", __func__, hash.ToString(), state.ToString());
            return false;
        }
        if (spare) {
            // Alone, nothing builds on it: refused (not the peer's fault) -- unless the node may need it
            // now: it would be the node's best header (more work than any it has: the chain the node
            // would follow, its block once it has it), or it is the tip an outbound peer (one the node
            // chose) announces, of a branch with more work than the active chain. Those, and one with a
            // header on top of it (the branch a block builds on), are taken as unchecked headers: the
            // peer's, within its budget, and kept for good only once the block is connected.
            if (!continued && !SpareWanted(block, *pindexPrev, source)) {
                LogDebug(BCLog::VALIDATION, "%s: spare header %s from peer=%d, alone (header-spare)\n", __func__, hash.ToString(), source);
                return state.Invalid(BlockValidationResult::BLOCK_HEADER_LOW_WORK, "header-spare", "the node keeps no more headers like this one, but on the branch of a header on top of it");
            }
            unchecked = true;
        }
        if (unchecked && min_pow_checked && !AdmitUnchecked(*pindexPrev, source)) {
            LogDebug(BCLog::VALIDATION, "%s: unchecked header %s from peer=%d over the budget (unchecked-budget)\n", __func__, hash.ToString(), source);
            return state.Invalid(BlockValidationResult::BLOCK_HEADER_LOW_WORK, "unchecked-budget", "the peer, or the node, holds as many headers it cannot check yet as it may");
        }
    }
    if (!min_pow_checked) {
        LogDebug(BCLog::VALIDATION, "%s: not adding new block header %s, missing anti-dos proof-of-work validation\n", __func__, hash.ToString());
        return state.Invalid(BlockValidationResult::BLOCK_HEADER_LOW_WORK, "too-little-chainwork");
    }
    // (AdmitUnchecked may have made room: the parent is not among what went.)
    CBlockIndex* pindex{m_blockman.AddToBlockIndex(block, m_best_header)};
    if (unchecked) {
        HoldUnchecked(*pindex, source);
    } else {
        sidechain::Hooks().HeaderKept(*pindex);
    }

    if (ppindex)
        *ppindex = pindex;

    return true;
}

// Exposed wrapper for AcceptBlockHeader
bool ChainstateManager::ProcessNewBlockHeaders(std::span<const CBlockHeader> headers, bool min_pow_checked, BlockValidationState& state, const CBlockIndex** ppindex, int64_t source)
{
    AssertLockNotHeld(cs_main);
    // (What is logged of the last header is read with the lock held: a sidechain's unchecked header may
    // leave the block index as soon as it is released, EraseUnchecked. So may *ppindex: the caller looks
    // it up again, by hash, if m_unchecked_erasures changed.)
    std::optional<std::pair<int, NodeClock::time_point>> last_accepted;
    {
        LOCK(cs_main);
        for (size_t i{0}; i < headers.size(); ++i) {
            const CBlockHeader& header{headers[i]};
            CBlockIndex *pindex = nullptr; // Use a temp pindex instead of ppindex to avoid a const_cast
            // (A header with the next one on top of it: the branch that one builds on, HeaderSpare.)
            const bool continued{i + 1 < headers.size() && headers[i + 1].hashPrevBlock == header.GetHash()};
            bool accepted{AcceptBlockHeader(header, state, &pindex, min_pow_checked, source, continued)};
            CheckBlockIndex();

            if (!accepted) {
                return false;
            }
            if (ppindex) {
                *ppindex = pindex;
            }
            if (pindex) last_accepted.emplace(pindex->nHeight, pindex->Time());
        }
    }
    if (NotifyHeaderTip()) {
        if (IsInitialBlockDownload() && ppindex && *ppindex && last_accepted) {
            int64_t blocks_left{(NodeClock::now() - last_accepted->second) / GetConsensus().PowTargetSpacing()};
            blocks_left = std::max<int64_t>(0, blocks_left);
            const double progress{100.0 * last_accepted->first / (last_accepted->first + blocks_left)};
            LogInfo("Synchronizing blockheaders, height: %d (~%.2f%%)\n", last_accepted->first, progress);
        }
    }
    return true;
}

void ChainstateManager::ReportHeadersPresync(int64_t height, int64_t timestamp)
{
    AssertLockNotHeld(GetMutex());
    {
        LOCK(GetMutex());
        // Don't report headers presync progress if we already have a post-minchainwork header chain.
        // This means we lose reporting for potentially legitimate, but unlikely, deep reorgs, but
        // prevent attackers that spam low-work headers from filling our logs.
        if (m_best_header->nChainWork >= UintToArith256(GetConsensus().nMinimumChainWork)) return;
        // Rate limit headers presync updates to 4 per second, as these are not subject to DoS
        // protection.
        auto now = MockableSteadyClock::now();
        if (now < m_last_presync_update + std::chrono::milliseconds{250}) return;
        m_last_presync_update = now;
    }
    bool initial_download = IsInitialBlockDownload();
    GetNotifications().headerTip(GetSynchronizationState(initial_download, m_blockman.m_blockfiles_indexed), height, timestamp, /*presync=*/true);
    if (initial_download) {
        int64_t blocks_left{(NodeClock::now() - NodeSeconds{std::chrono::seconds{timestamp}}) / GetConsensus().PowTargetSpacing()};
        blocks_left = std::max<int64_t>(0, blocks_left);
        const double progress{100.0 * height / (height + blocks_left)};
        LogInfo("Pre-synchronizing blockheaders, height: %d (~%.2f%%)\n", height, progress);
    }
}

/** Store block on disk. If dbp is non-nullptr, the file is known to already reside on disk */
bool ChainstateManager::AcceptBlock(const std::shared_ptr<const CBlock>& pblock, BlockValidationState& state, CBlockIndex** ppindex, bool fRequested, const FlatFilePos* dbp, bool* fNewBlock, bool min_pow_checked, int64_t source)
{
    const CBlock& block = *pblock;

    if (fNewBlock) *fNewBlock = false;
    AssertLockHeld(cs_main);

    CBlockIndex *pindexDummy = nullptr;
    CBlockIndex *&pindex = ppindex ? *ppindex : pindexDummy;

    bool accepted_header{AcceptBlockHeader(block, state, &pindex, min_pow_checked, source)};
    CheckBlockIndex();

    if (!accepted_header)
        return false;

    // The extension of an extended header carries what the block hash does not cover (a signature):
    // a block for a header the node has must carry the extension that header came with, the one its
    // rules checked. Another one is a mutated block, never a reason to take the header for invalid --
    // unless the sidechain's rules find it as valid (another signature of the same block): the block
    // is then the same, taken with the extension the node has. One the rules cannot tell yet (the
    // header was taken unchecked) is the same block too, taken with the header's extension, which is
    // checked when the block is connected: refusing the block would stall a branch that needs it to
    // be connected, and so to tell. (A valid one where the node's is not, or any other where the
    // node's failed, replaced the node's already: AcceptBlockHeader.)
    if (!sidechain::SameExtension(block, *pindex)) {
        switch (sidechain::Hooks().CheckOtherExtension(block, *pindex)) {
        case sidechain::OtherExtension::EQUIVALENT:
        case sidechain::OtherExtension::UNKNOWN:
        case sidechain::OtherExtension::REPLACE: {
            LogDebug(BCLog::VALIDATION, "%s: block %s carries another extension than its header's, as valid or that cannot be checked yet: taken with its header's\n", __func__, pindex->GetBlockHash().ToString());
            auto same{std::make_shared<CBlock>(block)};
            same->ext = *Assert(pindex->m_ext);
            return AcceptBlock(same, state, ppindex, fRequested, dbp, fNewBlock, min_pow_checked, source);
        }
        case sidechain::OtherExtension::INVALID:
            break;
        }
        LogDebug(BCLog::VALIDATION, "%s: block %s does not carry the extension of its header\n", __func__, pindex->GetBlockHash().ToString());
        return state.Invalid(BlockValidationResult::BLOCK_MUTATED, "bad-blk-extension", "the extension is not that of the header the node has");
    }

    // Check all requested blocks that we do not already have for validity and
    // save them to disk. Skip processing of unrequested blocks as an anti-DoS
    // measure, unless the blocks have more work than the active chain tip, and
    // aren't too far ahead of it, so are likely to be attached soon.
    bool fAlreadyHave = pindex->nStatus & BLOCK_HAVE_DATA;
    bool fHasMoreOrSameWork = (ActiveTip() ? pindex->nChainWork >= ActiveTip()->nChainWork : true);
    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    bool fTooFarAhead{pindex->nHeight > ActiveHeight() + int(MIN_BLOCKS_TO_KEEP)};

    // TODO: Decouple this function from the block download logic by removing fRequested
    // This requires some new chain data structure to efficiently look up if a
    // block is in a chain leading to a candidate for best tip, despite not
    // being such a candidate itself.
    // Note that this would break the getblockfrompeer RPC

    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.
    if (fAlreadyHave) return true;
    if (!fRequested) {  // If we didn't ask for it:
        if (pindex->nTx != 0) return true;    // This is a previously-processed block that was pruned
        if (!fHasMoreOrSameWork) return true; // Don't process less-work chains
        if (fTooFarAhead) return true;        // Block height is too high

        // Protect against DoS attacks from low-work chains.
        // If our tip is behind, a peer could try to send us
        // low-work blocks on a fake chain that we would never
        // request; don't process these.
        if (pindex->nChainWork < MinimumChainWork()) return true;
    }

    const CChainParams& params{GetParams()};

    if (!CheckBlock(block, state, params.GetConsensus()) ||
        !ContextualCheckBlock(block, state, *this, pindex->pprev)) {
        if (Assume(state.IsInvalid())) {
            ActiveChainstate().InvalidBlockFound(pindex, state);
        }
        LogError("%s: %s\n", __func__, state.ToString());
        return false;
    }

    // Header is valid/has work, merkle tree and segwit merkle tree are good...RELAY NOW
    // (but if it does not build on our best tip, let the SendMessages loop relay it)
    if (!IsInitialBlockDownload() && ActiveTip() == pindex->pprev && m_options.signals) {
        m_options.signals->NewPoWValidBlock(pindex, pblock);
    }

    // The block of an unchecked header is stored apart, in a file of its own, deleted with the header
    // (or moved to the block files once connected), within the budget of its sender (it holds the
    // header): a peer cannot fill the disk by sending blocks of headers it then takes away, nor by
    // sending the same one again after reconnecting. Over the budget, it is not stored; no fault of
    // the peer's.
    const bool apart{pindex->m_unchecked && !dbp};
    if (apart && !AdmitUncheckedBlock(*pindex, ::GetSerializeSize(TX_WITH_WITNESS(block)), source)) {
        LogDebug(BCLog::VALIDATION, "%s: block %s of an unchecked header from peer=%d over the budget: not stored\n", __func__, pindex->GetBlockHash().ToString(), source);
        return true;
    }

    // Write block to history file
    if (fNewBlock) *fNewBlock = true;
    try {
        FlatFilePos blockPos{};
        if (dbp) {
            blockPos = *dbp;
            m_blockman.UpdateBlockInfo(block, pindex->nHeight, blockPos);
        } else if (apart) {
            blockPos = m_blockman.WriteVolatileBlock(block);
            if (blockPos.IsNull()) {
                UnchargeUncheckedBlock(*pindex);
                state.Error(strprintf("%s: Failed to write the block of an unchecked header", __func__));
                return false;
            }
        } else {
            blockPos = m_blockman.WriteBlock(block, pindex->nHeight);
            if (blockPos.IsNull()) {
                state.Error(strprintf("%s: Failed to find position to write new block to disk", __func__));
                return false;
            }
        }
        ReceivedBlockTransactions(block, pindex, blockPos);
    } catch (const std::runtime_error& e) {
        return FatalError(GetNotifications(), state, strprintf(_("System error while saving block to disk: %s"), e.what()));
    }

    // TODO: FlushStateToDisk() handles flushing of both block and chainstate
    // data, so we should move this to ChainstateManager so that we can be more
    // intelligent about how we flush.
    // For now, since FlushStateMode::NONE is used, all that can happen is that
    // the block files may be pruned, so we can just call this on one
    // chainstate (particularly if we haven't implemented pruning with
    // background validation yet).
    //
    // Flush errors (e.g. low disk space during pruning) are ignored, so that
    // callers can't mistreat a flush failure as a block validation failure.
    // The fatal error notification inside FlushStateToDisk still fires,
    // so the node will shut down on unrecoverable flush errors regardless.
    // For state a dummy value is used, and the return value is ignored.
    BlockValidationState flush_state_ignore;
    (void)ActiveChainstate().FlushStateToDisk(flush_state_ignore, FlushStateMode::NONE);

    CheckBlockIndex();

    return true;
}

/**
 * A block for an extended header the node has, with another extension the sidechain's rules find as
 * valid (ChainHooks::CheckOtherExtension): the same block with the header's extension, the one the
 * node keeps. Any other block as it is.
 */
static std::shared_ptr<const CBlock> WithHeaderExtension(const ChainstateManager& chainman, const std::shared_ptr<const CBlock>& block) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (!block->IsExtended()) return block;
    const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(block->GetHash())};
    if (!index || !index->m_ext || sidechain::SameExtension(*block, *index)) return block;
    if (sidechain::Hooks().CheckOtherExtension(*block, *index) != sidechain::OtherExtension::EQUIVALENT) return block;
    auto same{std::make_shared<CBlock>(*block)};
    same->ext = *index->m_ext;
    return same;
}

bool ChainstateManager::ProcessNewBlock(const std::shared_ptr<const CBlock>& block_in, bool force_processing, bool min_pow_checked, bool* new_block, int64_t source)
{
    AssertLockNotHeld(cs_main);

    // (What is connected and announced is the block as the node keeps it.)
    std::shared_ptr<const CBlock> block{block_in};
    {
        CBlockIndex *pindex = nullptr;
        if (new_block) *new_block = false;
        BlockValidationState state;

        // CheckBlock() does not support multi-threaded block validation because CBlock::fChecked can cause data race.
        // Therefore, the following critical section must include the CheckBlock() call as well.
        LOCK(cs_main);
        block = WithHeaderExtension(*this, block);

        // Skipping AcceptBlock() for CheckBlock() failures means that we will never mark a block as invalid if
        // CheckBlock() fails.  This is protective against consensus failure if there are any unknown forms of block
        // malleability that cause CheckBlock() to fail; see e.g. CVE-2012-2459 and
        // https://lists.linuxfoundation.org/pipermail/bitcoin-dev/2019-February/016697.html.  Because CheckBlock() is
        // not very expensive, the anti-DoS benefits of caching failure (of a definitely-invalid block) are not substantial.
        bool ret = CheckBlock(*block, state, GetConsensus());
        if (ret) {
            // Store to disk (the block's header, if it is an unchecked one, is held by the source)
            ret = AcceptBlock(block, state, &pindex, force_processing, nullptr, new_block, min_pow_checked, source);
        }
        if (!ret) {
            if (m_options.signals) {
                m_options.signals->BlockChecked(block, state);
            }
            LogError("%s: AcceptBlock FAILED (%s)\n", __func__, state.ToString());
            return false;
        }
    }

    NotifyHeaderTip();

    BlockValidationState state; // Only used to report errors, not invalidity - ignore it
    if (!ActiveChainstate().ActivateBestChain(state, block)) {
        LogError("%s: ActivateBestChain failed (%s)\n", __func__, state.ToString());
        return false;
    }

    Chainstate* bg_chain{WITH_LOCK(cs_main, return HistoricalChainstate())};
    BlockValidationState bg_state;
    if (bg_chain && !bg_chain->ActivateBestChain(bg_state, block)) {
        LogError("%s: [background] ActivateBestChain failed (%s)\n", __func__, bg_state.ToString());
        return false;
     }

    return true;
}

MempoolAcceptResult ChainstateManager::ProcessTransaction(const CTransactionRef& tx, bool test_accept)
{
    AssertLockHeld(cs_main);
    Chainstate& active_chainstate = ActiveChainstate();
    if (!active_chainstate.GetMempool()) {
        TxValidationState state;
        state.Invalid(TxValidationResult::TX_NO_MEMPOOL, "no-mempool");
        return MempoolAcceptResult::Failure(state);
    }
    auto result = AcceptToMemoryPool(active_chainstate, tx, GetTime(), /*bypass_limits=*/ false, test_accept);
    active_chainstate.GetMempool()->check(active_chainstate.CoinsTip(), active_chainstate.m_chain.Height() + 1);
    return result;
}


BlockValidationState TestBlockValidity(
    Chainstate& chainstate,
    const CBlock& block,
    const bool check_pow,
    const bool check_merkle_root)
{
    // Lock must be held throughout this function for two reasons:
    // 1. We don't want the tip to change during several of the validation steps
    // 2. To prevent a CheckBlock() race condition for fChecked, see ProcessNewBlock()
    AssertLockHeld(chainstate.m_chainman.GetMutex());

    BlockValidationState state;
    CBlockIndex* tip{Assert(chainstate.m_chain.Tip())};

    if (block.hashPrevBlock != *Assert(tip->phashBlock)) {
        state.Invalid({}, "inconclusive-not-best-prevblk");
        return state;
    }

    // A block of a sidechain is checked before the mainchain is asked to
    // commit to it, as if the next mainchain block did.
    std::optional<sidechain::Mainchain::AssumeCommitted> assume_bmm;
    if (chainstate.m_chainman.m_mainchain) assume_bmm.emplace(*chainstate.m_chainman.m_mainchain, chainstate.m_chainman.m_mainchain->Height() + 1);

    // For signets CheckBlock() verifies the challenge iff fCheckPow is set.
    if (!CheckBlock(block, state, chainstate.m_chainman.GetConsensus(), /*fCheckPow=*/check_pow, /*fCheckMerkleRoot=*/check_merkle_root)) {
        // This should never happen, but belt-and-suspenders don't approve the
        // block if it does.
        if (state.IsValid()) NONFATAL_UNREACHABLE();
        return state;
    }

    /**
     * At this point ProcessNewBlock would call AcceptBlock(), but we
     * don't want to store the block or its header. Run individual checks
     * instead:
     * - skip AcceptBlockHeader() because:
     *   - we don't want to update the block index
     *   - we do not care about duplicates
     *   - we already ran CheckBlockHeader() via CheckBlock()
     *   - we already checked for prev-blk-not-found
     *   - we know the tip is valid, so no need to check bad-prevblk
     * - we already ran CheckBlock()
     * - do run ContextualCheckBlockHeader()
     * - do run ContextualCheckBlock()
     */

    if (!ContextualCheckBlockHeader(block, state, chainstate.m_chainman, tip)) {
        if (state.IsValid()) NONFATAL_UNREACHABLE();
        return state;
    }

    if (!ContextualCheckBlock(block, state, chainstate.m_chainman, tip)) {
        if (state.IsValid()) NONFATAL_UNREACHABLE();
        return state;
    }

    // We don't want ConnectBlock to update the actual chainstate, so create
    // a cache on top of it, along with a dummy block index.
    CBlockIndex index_dummy{block};
    uint256 block_hash(block.GetHash());
    index_dummy.pprev = tip;
    index_dummy.nHeight = tip->nHeight + 1;
    index_dummy.phashBlock = &block_hash;
    CCoinsViewCache view_dummy(&chainstate.CoinsTip());

    // Set fJustCheck to true in order to update, and not clear, validation caches.
    if(!chainstate.ConnectBlock(block, state, &index_dummy, view_dummy, /*fJustCheck=*/true)) {
        if (state.IsValid()) NONFATAL_UNREACHABLE();
        return state;
    }

    // Ensure no check returned successfully while also setting an invalid state.
    if (!state.IsValid()) NONFATAL_UNREACHABLE();

    return state;
}

/* This function is called from the RPC code for pruneblockchain */
void PruneBlockFilesManual(Chainstate& active_chainstate, int nManualPruneHeight)
{
    BlockValidationState state;
    if (!active_chainstate.FlushStateToDisk(
            state, FlushStateMode::NONE, nManualPruneHeight)) {
        LogWarning("Failed to flush state after manual prune (%s)", state.ToString());
    }
}

bool Chainstate::LoadChainTip()
{
    AssertLockHeld(cs_main);
    const CCoinsViewCache& coins_cache = CoinsTip();
    assert(!coins_cache.GetBestBlock().IsNull()); // Never called when the coins view is empty
    CBlockIndex* tip = m_chain.Tip();

    if (tip && tip->GetBlockHash() == coins_cache.GetBestBlock()) {
        return true;
    }

    // Load pointer to end of best chain
    CBlockIndex* pindex = m_blockman.LookupBlockIndex(coins_cache.GetBestBlock());
    if (!pindex) {
        return false;
    }
    m_chain.SetTip(*pindex);
    m_chainman.UpdateIBDStatus();
    m_last_flushed_block = pindex;
    tip = m_chain.Tip();

    // nSequenceId is one of the keys used to sort setBlockIndexCandidates. Ensure all
    // candidate sets are empty to avoid UB, as nSequenceId is about to be modified.
    for (const auto& cs : m_chainman.m_chainstates) {
        assert(cs->setBlockIndexCandidates.empty());
    }

    // Make sure our chain tip before shutting down scores better than any other candidate
    // to maintain a consistent best tip over reboots in case of a tie.
    auto target = tip;
    while (target) {
        target->nSequenceId = SEQ_ID_BEST_CHAIN_FROM_DISK;
        target = target->pprev;
    }

    LogInfo("Loaded best chain: hashBestChain=%s height=%d date=%s progress=%f",
              tip->GetBlockHash().ToString(),
              m_chain.Height(),
              FormatISO8601DateTime(tip->GetBlockTime()),
              m_chainman.GuessVerificationProgress(tip));

    // Ensure KernelNotifications m_tip_block is set even if no new block arrives.
    if (!this->GetRole().historical) {
        // Ignoring return value for now.
        (void)m_chainman.GetNotifications().blockTip(
            /*state=*/GetSynchronizationState(/*init=*/true, m_chainman.m_blockman.m_blockfiles_indexed),
            /*index=*/*pindex,
            /*verification_progress=*/m_chainman.GuessVerificationProgress(tip));
    }

    CheckForkWarningConditions();

    return true;
}

CVerifyDB::CVerifyDB(Notifications& notifications)
    : m_notifications{notifications}
{
    m_notifications.progress(_("Verifying blocks…"), 0, false);
}

CVerifyDB::~CVerifyDB()
{
    m_notifications.progress(bilingual_str{}, 100, false);
}

drivechain::SidechainDB Chainstate::GetMempoolSidechainDB(const std::set<drivechain::SidechainId>& slots, const std::set<Txid>* stop_before, std::set<Txid>* applied) const
{
    AssertLockHeld(::cs_main);
    // Only the sidechains asked about: a copy of the whole database for every transaction that
    // touches an escrow would cost as much as the database is large.
    drivechain::SidechainDB scdb{m_scdb.Subset(slots)};
    if (!m_mempool) return scdb;
    AssertLockHeld(m_mempool->cs);

    const Consensus::DrivechainParams& params{m_chainman.GetConsensus().drivechain};
    drivechain::SidechainDB::EscrowOutputs escrow_outputs{scdb.GetEscrowOutputs()};
    drivechain::BlockUndo undo;
    std::string reject_reason;

    for (const drivechain::SidechainId id : slots) {
        while (true) {
            const drivechain::Slot* slot{scdb.GetSlot(id)};
            if (!slot) break;
            CTransactionRef next;
            if (slot->has_ctip) {
                // The deposit or withdrawal that spends the escrow output.
                if (const CTransaction* spender{m_mempool->GetConflictTx(slot->ctip.outpoint)}) next = m_mempool->get(spender->GetHash());
            } else if (const auto txs{m_mempool->m_escrow_txs.find(id)}; txs != m_mempool->m_escrow_txs.end()) {
                // No escrow output yet: the deposit that creates the first one.
                for (const Txid& txid : txs->second) {
                    const CTransactionRef candidate{m_mempool->get(txid)};
                    if (!candidate) continue;
                    // The first deposit spends no escrow output in the mempool; one that does comes
                    // later in the chain, whatever its txid.
                    if (std::any_of(candidate->vin.begin(), candidate->vin.end(), [&](const CTxIn& in) { return txs->second.contains(in.prevout.hash); })) continue;
                    // Nor one whose inputs are missing: in a reorg, the deposits of the disconnected
                    // blocks come back one by one, and a later one that stayed in the mempool waits
                    // for the one before it (which would otherwise be refused, as not following it).
                    if (std::any_of(candidate->vin.begin(), candidate->vin.end(), [&](const CTxIn& in) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return !m_mempool->exists(in.prevout.hash) && !m_coins_views->m_cacheview->HaveCoin(in.prevout); })) continue;
                    drivechain::SidechainDB probe{scdb};
                    drivechain::SidechainDB::EscrowOutputs probe_outputs{escrow_outputs};
                    drivechain::BlockUndo probe_undo;
                    if (probe.ConnectTx(*candidate, params, probe_outputs, probe_undo, nullptr, reject_reason)) {
                        next = candidate;
                        break;
                    }
                }
            }
            if (!next) break;
            if (stop_before && stop_before->contains(next->GetHash())) break;
            if (!scdb.ConnectTx(*next, params, escrow_outputs, undo, nullptr, reject_reason)) break;
            if (applied) applied->insert(next->GetHash());
        }
    }
    return scdb;
}

void Chainstate::NoteSideChanges(const std::map<std::vector<unsigned char>, std::optional<std::vector<unsigned char>>>& changes)
{
    AssertLockHeld(::cs_main);
    // (Only for the mempool's checks: none without one.)
    if (!m_mempool || m_side_changed_all) return;
    for (const auto& [key, value] : changes) m_side_changed.insert(key);
    // Bounded: past so many (blocks connected without a check between), all.
    if (m_side_changed.size() > MAX_SIDE_CHANGES_NOTED) {
        m_side_changed.clear();
        m_side_changed_all = true;
    }
}

void Chainstate::RemoveStaleDrivechainTxs(bool after_reorg)
{
    AssertLockHeld(::cs_main);
    // What changed since the last call: taken whatever happens next.
    const std::set<std::vector<unsigned char>> changed{std::exchange(m_side_changed, {})};
    const bool changed_all{std::exchange(m_side_changed_all, false)};
    if (!m_mempool) return;
    AssertLockHeld(m_mempool->cs);
    const CBlockIndex* tip{m_chain.Tip()};
    if (!tip) return;

    // zSide: whether the shielded transactions are looked at now. Anchors only go when blocks are
    // undone. In the middle of a reorg the state is one on the way to the new tip, against which valid
    // transactions could fail: they are looked at once it is done (MaybeUpdateMempoolForReorg).
    bool anchors_went{false};
    if (after_reorg || !m_blocks_disconnected) {
        anchors_went = m_blocks_disconnected;
        m_blocks_disconnected = false;
    }

    // (On a sidechain the hooks' own rules are checked again whatever else is in the mempool.)
    if (!m_chainman.GetConsensus().sidechain.enabled && m_mempool->m_bmm_requests.empty() && m_mempool->m_escrow_txs.empty() && m_mempool->m_refunds.empty() && m_mempool->m_shielded_nullifiers.empty()) return;

    std::vector<CTransactionRef> stale;

    // On a sidechain: a withdrawal that was paid, refunded or put in a bundle can no longer be taken back.
    if (m_chainman.GetConsensus().sidechain.enabled) {
        // Nor while a bundle of this sidechain is pending on the mainchain.
        const sidechain::State side{SideState()};
        const bool main_pending{side.MainPendingNext(*Assert(m_chainman.m_mainchain), m_chainman.GetConsensus().sidechain)};
        for (const auto& [withdrawal, txid] : m_mempool->m_refunds) {
            if (side.GetWithdrawal(withdrawal) && !side.InBundle(withdrawal) && !main_pending) continue;
            if (const CTransactionRef tx{m_mempool->get(txid)}) stale.push_back(tx);
        }
        // Shielded transactions whose anchor a reorg took away: a block with them would be invalid,
        // and no block could be built. (Those whose notes another spent in a block, revealing one of
        // their nullifiers, went with the block's conflicts: CTxMemPool::removeForBlock.)
        std::set<Txid> checked;
        for (const auto& [nf, txid] : m_mempool->m_shielded_nullifiers) {
            if (!anchors_went) break;
            if (!checked.insert(txid).second) continue;
            const CTransactionRef tx{m_mempool->get(txid)};
            if (!tx) continue;
            std::string error;
            const auto bundle{zside::Bundle::FromTx(*tx, error)};
            if (bundle && !SideState().Shielded().IsAnchor(bundle->Anchor())) stale.push_back(tx);
        }
        // The sidechain's own rules (ChainHooks::HasTxRules), checked at entry against the tip then:
        // a block may have changed the state they read (a name or an asset taken by another transaction
        // that was mined first, say). Such a transaction no block takes would otherwise hold its
        // inputs, and the wallet that made it, until the mempool expires it. Only those whose state
        // changed since the last check (TxRuleKeys; all of them if the hooks cannot tell, or the state
        // was loaded): the work is that of what blocks changed, not that of the mempool, which anyone
        // can fill with transactions whose rules cost signature checks. And at most
        // MAX_RULE_CHECKS_PER_TIP at each change of the tip, the rest queued for the next ones: one
        // who has a key that many transactions read change at every block (a bond of its own mined)
        // makes no more work than that. (Only the mempool's upkeep: block assembly checks the rules
        // of each transaction it takes anyway.)
        for (const Txid& txid : m_mempool->m_rule_txs_any) m_mempool->QueueRuleCheck(txid);
        if (changed_all) {
            for (const auto& [txid, keys] : m_mempool->m_rule_keys_of) m_mempool->QueueRuleCheck(txid);
        } else {
            for (const auto& key : changed) {
                if (const auto found{m_mempool->m_rule_txs.find(key)}; found != m_mempool->m_rule_txs.end()) {
                    for (const Txid& txid : found->second) m_mempool->QueueRuleCheck(txid);
                }
            }
        }
        const int next_height{m_chain.Height() + 1};
        for (const Txid& txid : m_mempool->TakeRuleChecks(MAX_RULE_CHECKS_PER_TIP)) {
            const CTransactionRef ptx{m_mempool->get(txid)};
            if (!ptx) continue;
            const CTransaction& tx{*ptx};
            sidechain::StoreOverlay scratch{side.View(), /*journal=*/false};
            sidechain::State scratch_state{scratch};
            std::vector<CTxOut> payouts;
            std::string reason;
            if (!sidechain::Hooks().ApplyTx(scratch_state, tx, next_height, /*peg_step=*/false, payouts, reason)) {
                LogDebug(BCLog::MEMPOOL, "%s no longer keeps the rules of the sidechain (%s): out of the mempool\n", tx.GetHash().ToString(), reason);
                stale.push_back(ptx);
            }
        }
    }

    // Requests for blind merged mining are bound to the block they were made for.
    for (const auto& [id, txid] : m_mempool->m_bmm_requests) {
        const CTransactionRef tx{m_mempool->get(txid)};
        if (!tx) continue;
        for (const drivechain::BmmRequest& request : drivechain::GetBmmRequests(*tx)) {
            if (request.prev_main_block_hash != tip->GetBlockHash()) {
                stale.push_back(tx);
                break;
            }
        }
    }

    // Deposits and withdrawals have to form a chain that starts at the escrow output of their sidechain.
    const uint32_t max_sidechains{m_chainman.GetConsensus().drivechain.max_sidechains};
    std::set<drivechain::SidechainId> slots;
    for (const auto& [id, txids] : m_mempool->m_escrow_txs) {
        if (id < max_sidechains) slots.insert(id);
    }
    std::set<Txid> valid;
    (void)GetMempoolSidechainDB(slots, nullptr, &valid);
    for (const drivechain::SidechainId id : slots) {
        for (const Txid& txid : m_mempool->m_escrow_txs.at(id)) {
            if (valid.contains(txid)) continue;
            if (const CTransactionRef tx{m_mempool->get(txid)}) stale.push_back(tx);
        }
    }

    for (const CTransactionRef& tx : stale) {
        m_mempool->removeRecursive(*tx, MemPoolRemovalReason::CONFLICT);
    }
}

std::string Chainstate::DrivechainStateName() const
{
    return m_from_snapshot_blockhash ? m_from_snapshot_blockhash->ToString() : std::string{};
}

sidechain::StoreOverlay& Chainstate::SideCache()
{
    AssertLockHeld(::cs_main);
    if (!m_side_cache) {
        m_side_db = m_blockman.m_drivechain_db->SideStore(DrivechainStateName());
        m_side_cache = std::make_unique<sidechain::StoreOverlay>(*m_side_db);
    }
    return *m_side_cache;
}

void Chainstate::WriteDrivechainState()
{
    AssertLockHeld(::cs_main);
    sidechain::StoreOverlay& cache{SideCache()};
    m_blockman.m_drivechain_db->WriteState(DrivechainStateName(), m_scdb, m_side_db.get(), &cache.Changes());
    cache.Clear();
}

void Chainstate::ReleaseSideCursor()
{
    AssertLockHeld(::cs_main);
    if (m_side_db) m_side_db->Reset();
}

util::Result<void> Chainstate::ResetDrivechainState()
{
    AssertLockHeld(::cs_main);
    sidechain::StoreOverlay& cache{SideCache()};  // opens the store
    m_side_changed_all = true;
    // Of another format, or derived under other parameters: wiped whole (the blocks found invalid
    // under the former parameters are judged again), and marked as of the current format at once:
    // what the blocks write from now on is of this one.
    bilingual_str error;
    if (!PrepareDrivechainDB(/*rebuild_from=*/nullptr, error)) return util::Error{error};
    m_blockman.m_drivechain_db->WipeState(DrivechainStateName(), *m_side_db);
    cache.Clear();
    m_scdb = drivechain::SidechainDB{};
    return {};
}

void Chainstate::EraseDrivechainUndo()
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* flushed{m_last_flushed_block};
    if (!flushed || !m_chain.Contains(*flushed)) return;
    const int last{flushed->nHeight - DRIVECHAIN_UNDO_DEPTH};
    std::vector<uint256> hashes;
    for (int height{std::max(0, m_drivechain_undo_erased_height + 1)}; height <= last; ++height) {
        hashes.push_back(m_chain[height]->GetBlockHash());
        // In batches: after a long initial sync without a flush there can be many.
        if (hashes.size() >= m_drivechain_undo_erase_batch) {
            m_blockman.m_drivechain_db->EraseBlockUndo(hashes);
            hashes.clear();
        }
    }
    if (!hashes.empty()) m_blockman.m_drivechain_db->EraseBlockUndo(hashes);
    m_drivechain_undo_erased_height = std::max(m_drivechain_undo_erased_height, last);
    // What the above does not reach (the undo data of blocks that left the chain, of blocks a crash
    // took out of it, of active blocks passed over after a reorg the node stopped in), now and then.
    if (flushed->nHeight >= m_drivechain_undo_swept_height + DRIVECHAIN_UNDO_SWEEP_INTERVAL) SweepDrivechainUndo(*flushed);
}

size_t Chainstate::ForgetUnreachableRecordFailures(int last)
{
    AssertLockHeld(::cs_main);
    sidechain::Mainchain* const mainchain{m_chainman.m_mainchain.get()};
    if (!mainchain || last < 0 || last > m_chain.Height()) return 0;
    const CBlockIndex* const fixed{m_chain[last]};
    std::vector<uint256> unreachable;
    for (const uint256& hash : mainchain->RecordFailures()) {
        const CBlockIndex* const index{m_blockman.LookupBlockIndex(hash)};
        // A block the index does not have (noted just before a stop, say): kept, nothing to judge it by.
        if (!index || m_chain.Contains(*index)) continue;
        // It leaves the active chain at or below `last` (it is there, or does not descend from the block
        // there): reaching it takes back blocks no reorg can.
        if (index->nHeight <= last || index->GetAncestor(last) != fixed) unreachable.push_back(hash);
    }
    return mainchain->ForgetRecordFailures(unreachable);
}

void Chainstate::SweepDrivechainUndo(const CBlockIndex& flushed)
{
    AssertLockHeld(::cs_main);
    m_drivechain_undo_swept_height = flushed.nHeight;
    const int last{flushed.nHeight - DRIVECHAIN_UNDO_DEPTH};
    if (const size_t forgotten{ForgetUnreachableRecordFailures(last)}) {
        LogDebug(BCLog::VALIDATION, "Forgot the failures against the record of %u blocks out of reach of any reorg", forgotten);
    }
    drivechain::Database& db{*m_blockman.m_drivechain_db};
    std::vector<uint256> erase;
    std::vector<uint256> stale;
    for (const uint256& hash : db.ListUndoBlocks()) {
        const CBlockIndex* pindex{m_blockman.LookupBlockIndex(hash)};
        // Out of reach of any reorg from the flushed block (reconnected, a block writes its undo data again).
        if (pindex && pindex->nHeight > last) continue;
        erase.push_back(hash);
        // A block out of the active chain does not need its deposit records either (readers skip them).
        if (!pindex || !m_chain.Contains(*pindex)) stale.push_back(hash);
    }
    if (erase.empty()) return;
    LogDebug(BCLog::COINDB, "Erasing the drivechain undo data of %u blocks out of reach (%u not in the active chain)", erase.size(), stale.size());
    db.EraseBlockUndo(erase);
    for (const uint256& hash : stale) db.EraseBlockDeposits(hash);
}

void Chainstate::ResetAllBlockFailureFlags()
{
    AssertLockHeld(::cs_main);
    size_t reset{0};
    for (auto& [hash, block_index] : m_blockman.m_block_index) {
        if (!(block_index.nStatus & BLOCK_FAILED_VALID)) continue;
        // A block the operator marked invalid (invalidateblock) stays so: no parameter changed that.
        if (m_chainman.m_mainchain && m_chainman.m_mainchain->GetFailure(hash) == sidechain::Mainchain::Failure::MANUAL) continue;
        block_index.nStatus &= ~BLOCK_FAILED_VALID;
        m_blockman.m_dirty_blockindex.insert(&block_index);
        ++reset;
    }
    if (reset == 0) return;
    LogInfo("%u blocks found invalid under the former drivechain parameters are judged again", reset);
    m_chainman.m_best_invalid = nullptr;
    CBlockIndex* best_header{nullptr};
    for (auto& [_, block_index] : m_blockman.m_block_index) {
        if (block_index.IsValid(BLOCK_VALID_TREE) && (!best_header || CBlockIndexWorkComparator()(best_header, &block_index))) {
            best_header = &block_index;
        }
    }
    if (best_header) m_chainman.m_best_header = best_header;
    // On disk before anything else changes: the database is not rebuilt again if the node stops
    // before its next flush, so neither would the flags be reset again.
    m_blockman.WriteBlockIndexDB();
}

bool Chainstate::RollBackFromInvalidBlock(CBlockIndex* invalid, drivechain::SidechainDB&& scdb, bilingual_str& error)
{
    AssertLockHeld(::cs_main);
    assert(invalid && invalid->pprev && m_chain.Contains(*invalid));
    assert(scdb.GetBlockHash() == invalid->pprev->GetBlockHash());
    LogWarning("Block %s at height %d of the active chain breaks the drivechain rules: the chainstate goes back to height %d",
               invalid->GetBlockHash().ToString(), invalid->nHeight, invalid->nHeight - 1);
    const bilingual_str reindex_error{strprintf(_("Block %s at height %d breaks the drivechain rules of this version, and the chainstate could not be taken back below it (see the log). "
                                                  "Restart with -reindex-chainstate (with -reindex on a pruned node)"),
                                                invalid->GetBlockHash().ToString(), invalid->nHeight)};
    // Like DisconnectTip without the sidechain database (already at the parent, the store of the
    // sidechain state with it), the mempool (not loaded yet: the transactions of the blocks go to it
    // later, ReaddRolledBackTransactions) or notifications (nothing listens yet: wallets and indexes
    // catch up from their own best block).
    // All in one view: if a block cannot be disconnected, nothing changed.
    CCoinsViewCache view(&CoinsTip());
    // Bounded like the pool of a reorg: past the bound, the most recently confirmed are dropped.
    DisconnectedBlockTransactions disconnected{MAX_DISCONNECTED_TX_POOL_BYTES};
    for (CBlockIndex* pindex{m_chain.Tip()}; pindex != invalid->pprev; pindex = pindex->pprev) {
        CBlock block;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA) || !(pindex->nStatus & BLOCK_HAVE_UNDO) || !m_blockman.ReadBlock(block, *pindex)) {
            LogError("%s: no block or undo data for block %s at height %d", __func__, pindex->GetBlockHash().ToString(), pindex->nHeight);
            error = reindex_error;
            disconnected.clear();
            return false;
        }
        if (DisconnectBlock(block, pindex, view) != DISCONNECT_OK) {
            LogError("%s: failed to disconnect block %s at height %d", __func__, pindex->GetBlockHash().ToString(), pindex->nHeight);
            error = reindex_error;
            disconnected.clear();
            return false;
        }
        (void)disconnected.AddTransactionsFromBlock(block.vtx);
    }
    view.Flush(/*reallocate_cache=*/false);
    for (CTransactionRef& tx : disconnected.take()) {
        if (!tx->IsCoinBase()) m_rolled_back_txs.push_back(std::move(tx));
    }
    m_chain.SetTip(*invalid->pprev);
    invalid->nStatus |= BLOCK_FAILED_VALID;
    m_blockman.m_dirty_blockindex.insert(invalid);
    InvalidChainFound(invalid);

    m_scdb = std::move(scdb);
    m_blockman.m_drivechain_db->WriteFormatVersion();
    // The coins, the block index and the snapshot of the sidechain database (with the store's
    // changes) together.
    ForceFlushStateToDisk();
    return true;
}

void Chainstate::ReaddRolledBackTransactions()
{
    LOCK(::cs_main);
    if (m_rolled_back_txs.empty()) return;
    const std::vector<CTransactionRef> txs{std::exchange(m_rolled_back_txs, {})};
    if (!m_mempool) return;
    LOCK(m_mempool->cs);
    // In the order of DisconnectedBlockTransactions (AddTransactionsFromBlock takes a block's
    // transactions last first): MaybeUpdateMempoolForReorg adds the earliest confirmed first.
    // Those already there (a wallet loaded meanwhile resubmits its own) are left alone: offered
    // again they would fail, and MaybeUpdateMempoolForReorg removes what fails, with what spends it.
    std::vector<CTransactionRef> missing;
    std::copy_if(txs.rbegin(), txs.rend(), std::back_inserter(missing), [&](const CTransactionRef& tx) { return !m_mempool->exists(tx->GetHash()); });
    DisconnectedBlockTransactions disconnectpool{MAX_DISCONNECTED_TX_POOL_BYTES};
    (void)disconnectpool.AddTransactionsFromBlock(missing);
    // Those in a block connected since (another branch), or that no longer pass, are left out.
    MaybeUpdateMempoolForReorg(disconnectpool, /*fAddToMempool=*/true);
    const size_t in_mempool{size_t(std::count_if(txs.begin(), txs.end(), [&](const CTransactionRef& tx) { return m_mempool->exists(tx->GetHash()); }))};
    LogInfo("%u of the %u transactions of the blocks taken back at startup returned to the mempool", in_mempool, txs.size());
}

bool Chainstate::PrepareDrivechainDB(const CBlockIndex* rebuild_from, bilingual_str& error)
{
    AssertLockHeld(::cs_main);
    drivechain::Database& db{*m_blockman.m_drivechain_db};
    const drivechain::Database::Format format{db.CheckFormat()};
    if (format == drivechain::Database::Format::CURRENT) return true;

    // Rebuilt from the blocks: those must all be there before anything is wiped (a pruned node no
    // longer has the old ones), or the node would be left with neither the old data nor the new.
    if (rebuild_from && m_blockman.m_have_pruned) {
        for (const CBlockIndex* pindex{rebuild_from}; pindex->pprev; pindex = pindex->pprev) {
            if (!(pindex->nStatus & BLOCK_HAVE_DATA)) {
                LogError("The sidechain database has to be rebuilt from the blocks, and block %s at height %d was pruned", pindex->GetBlockHash().ToString(), pindex->nHeight);
                error = strprintf(_("The sidechain database has to be rebuilt from the blocks (it is %s), and this pruned node no longer has block %d. Nothing was changed: "
                                    "restart with -reindex to download the blocks again, or go back to the former version"),
                                  format == drivechain::Database::Format::OTHER_PARAMS ? _("derived under other drivechain parameters") : _("in an older format"),
                                  pindex->nHeight);
                return false;
            }
        }
    }

    switch (format) {
    case drivechain::Database::Format::CURRENT:
        break;
    case drivechain::Database::Format::OTHER_VERSION:
        // Laid out another way (by an older version, before the sidechain state had a store of its
        // own say): built anew from the blocks.
        LogInfo("The sidechain database is in an older format; it is rebuilt from the blocks");
        break;
    case drivechain::Database::Format::OTHER_PARAMS:
        // Derived under other drivechain parameters (a period that changed, say): the
        // snapshot, the undo data, the events and the deposit index may all differ under these.
        LogInfo("The sidechain database was derived under other drivechain parameters; it is rebuilt from the blocks");
        // So may the validity of blocks: those found invalid under the old rules are judged again.
        ResetAllBlockFailureFlags();
        break;
    }
    // Everything goes, the state of this chain as a sidechain with it (and the iterator and the
    // changes in memory of its store, which no longer match).
    sidechain::StoreOverlay& cache{SideCache()};
    db.Wipe();
    m_side_db->Reset();
    cache.Clear();
    m_side_changed_all = true;
    // Without a chainstate to rebuild from (-reindex-chainstate, say), the blocks connected next
    // fill the database in the current format.
    if (!rebuild_from) db.WriteFormatVersion();
    return true;
}

util::Result<void> Chainstate::LoadDrivechainState()
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* tip{m_chain.Tip()};
    if (!tip) return {};
    // The tip is the flushed block: the undo data below it out of reach was erased then (or is
    // erased below by the roll forward).
    m_drivechain_undo_erased_height = tip->nHeight - DRIVECHAIN_UNDO_DEPTH;

    drivechain::Database& db{*m_blockman.m_drivechain_db};
    sidechain::StoreOverlay& cache{SideCache()};
    m_side_changed_all = true;
    // Of another format, or derived under other parameters: wiped, after checking that the blocks
    // it is derived again from are all there (nothing is wiped otherwise).
    const bool old_format{db.CheckFormat() != drivechain::Database::Format::CURRENT};
    if (bilingual_str error; !PrepareDrivechainDB(/*rebuild_from=*/tip, error)) return util::Error{error};

    drivechain::SidechainDB scdb;
    // On failure ReadState leaves `scdb` empty: nothing half read is used.
    const bool have_state{db.ReadState(DrivechainStateName(), scdb)};
    if (have_state && scdb.GetBlockHash() == tip->GetBlockHash()) {
        m_scdb = std::move(scdb);
        db.WriteFormatVersion();
        SweepDrivechainUndo(*tip);
        return {};
    }

    const CBlockIndex* pindex{!have_state || scdb.GetBlockHash().IsNull() ? nullptr : m_blockman.LookupBlockIndex(scdb.GetBlockHash())};
    if (m_from_snapshot_blockhash && (!pindex || tip->GetAncestor(pindex->nHeight) != pindex)) {
        // A chainstate created from a UTXO snapshot does not have the blocks the sidechain database
        // would have to be derived from. One written since, at a block of the chain, is brought
        // forward to the tip below; without one, it starts empty.
        LogWarning("No sidechain database for the chainstate loaded from a UTXO snapshot; starting with an empty one.");
        db.WipeState(DrivechainStateName(), *m_side_db);
        cache.Clear();
        m_scdb = drivechain::SidechainDB{};
        m_scdb.SetBlockHash(tip->GetBlockHash());
        db.WriteFormatVersion();
        return {};
    }

    if (pindex) {
        // Rewind to the last block the snapshot has in common with the chain. The blocks on the way
        // are not in the active chain, so their undo data was kept (only blocks deep in the active
        // chain lose theirs); if it is missing all the same, everything is derived from the blocks.
        // The changes to the store are made in an overlay, dropped if it comes to that. (The deposit
        // records of those blocks stay: readers skip blocks out of the active chain.)
        sidechain::StoreOverlay rewind{cache, /*journal=*/false};
        while (tip->GetAncestor(pindex->nHeight) != pindex) {
            drivechain::BlockUndo undo;
            if (!db.ReadBlockUndo(pindex->GetBlockHash(), undo)) {
                LogWarning("No drivechain undo data for block %s; the sidechain database is rebuilt from the blocks", pindex->GetBlockHash().ToString());
                pindex = nullptr;
                break;
            }
            scdb.DisconnectBlock(undo);
            rewind.Revert(undo.side);
            pindex = pindex->pprev;
        }
        if (pindex) rewind.MergeInto(cache);
    }
    const bool from_genesis{!pindex};
    if (from_genesis) {
        // No usable snapshot -- none yet, or one of an older format: derive everything from the
        // blocks, the store emptied first.
        LogInfo("Deriving the sidechain database from the blocks");
        db.WipeState(DrivechainStateName(), *m_side_db);
        cache.Clear();
        scdb = drivechain::SidechainDB{};
        pindex = m_chain.Genesis();
        scdb.SetBlockHash(pindex->GetBlockHash());
    }
    // The blocks it is brought forward with have to be there: a pruned node may not have them.
    for (const CBlockIndex* block{tip}; block != pindex; block = block->pprev) {
        if (!(block->nStatus & BLOCK_HAVE_DATA)) {
            if (old_format) {
                return util::Error{_("The sidechain state of this node is in a format an earlier version wrote, or was derived under other drivechain "
                                     "parameters, and has to be derived again from the blocks, which this node pruned. Restart with -reindex to download them again.")};
            }
            return util::Error{strprintf(_("The sidechain state of this node is as of block %d, and the blocks it has to be brought forward with "
                                           "are pruned (block %d is missing). Restart with -reindex to download them again."), pindex->nHeight, block->nHeight)};
        }
    }
    LogInfo("Bringing the sidechain database from height %d to the chain tip at height %d", pindex->nHeight, tip->nHeight);

    // Roll forward along the chain, with undo data for the blocks a reorg can still take back.
    const CBlockIndex* stopped{nullptr};
    const CBlockIndex* invalid{nullptr};
    if (auto result{RollForwardSidechainDB(scdb, pindex, tip, tip->nHeight - DRIVECHAIN_UNDO_DEPTH, &cache, m_from_snapshot_blockhash ? nullptr : &stopped, &invalid)}; !result) {
        if (invalid) {
            // A block of the active chain breaks the rules of this version whatever the record says
            // (they changed since it was connected): the chainstate goes back to its parent, where the
            // sidechain database and the store now are, and the block is marked invalid so that the
            // node can move to another chain.
            if (bilingual_str error; !RollBackFromInvalidBlock(m_blockman.LookupBlockIndex(invalid->GetBlockHash()), std::move(scdb), error)) return util::Error{error};
            return {};
        }
        if (m_chainman.m_interrupt && !scdb.GetBlockHash().IsNull()) {
            // Interrupted (a shutdown): what was derived so far is kept, the store with it, and the
            // next start goes on from there rather than from the beginning (the snapshot of a block
            // of the active chain is rolled forward from).
            LogInfo("Interrupted while bringing the sidechain database to the chain tip; it goes on from block %s next time", scdb.GetBlockHash().ToString());
            db.WriteState(DrivechainStateName(), scdb, m_side_db.get(), &cache.Changes());
            cache.Clear();
            db.WriteFormatVersion();
        }
        return result;
    }
    if (stopped) {
        // A block that fails against the record of the mainchain as it is now (the record changed
        // while this node was down, say), or that waits for it: the chain goes back to the block
        // before it, where the sidechain database is. Connecting the rest again is left to
        // ActivateBestChain, which marks what fails as failed against the record, and to the
        // follower, which takes that back when the record changes. Not marked invalid here: the
        // record may change.
        const CBlockIndex* parent{stopped->pprev};
        LogWarning("Block %s at height %d does not fit the record of the mainchain as it is; the chain goes back to height %d, to be checked again",
                   stopped->GetBlockHash().ToString(), stopped->nHeight, parent->nHeight);
        CCoinsViewCache view{&CoinsTip()};
        // Their transactions are offered to the mempool again (ReaddRolledBackTransactions), as after
        // RollBackFromInvalidBlock: the blocks may never connect again.
        DisconnectedBlockTransactions disconnected{MAX_DISCONNECTED_TX_POOL_BYTES};
        for (const CBlockIndex* block_index{tip}; block_index != parent; block_index = block_index->pprev) {
            CBlock block;
            if (!m_blockman.ReadBlock(block, *block_index)) {
                disconnected.clear();
                return util::Error{strprintf(_("Failed to read block %s. Restart with -reindex."), block_index->GetBlockHash().ToString())};
            }
            // The coins only: the sidechain database never got to this block.
            if (DisconnectBlock(block, block_index, view) != DISCONNECT_OK) {
                disconnected.clear();
                return util::Error{strprintf(_("Failed to take block %s back. Restart with -reindex-chainstate."), block_index->GetBlockHash().ToString())};
            }
            (void)disconnected.AddTransactionsFromBlock(block.vtx);
        }
        view.Flush(/*reallocate_cache=*/false);
        for (CTransactionRef& tx : disconnected.take()) {
            if (!tx->IsCoinBase()) m_rolled_back_txs.push_back(std::move(tx));
        }
        m_chain.SetTip(*const_cast<CBlockIndex*>(parent));
        m_scdb = std::move(scdb);
        WriteDrivechainState();
        CoinsTip().Flush();
        db.WriteFormatVersion();
        return {};
    }

    m_scdb = std::move(scdb);
    WriteDrivechainState();
    db.WriteFormatVersion();
    SweepDrivechainUndo(*tip);
    return {};
}

util::Result<void> Chainstate::RollForwardSidechainDB(drivechain::SidechainDB& scdb, const CBlockIndex* from, const CBlockIndex* to, int keep_undo_above,
                                                      sidechain::StoreOverlay* side_store, const CBlockIndex** stopped_at, const CBlockIndex** invalid)
{
    AssertLockHeld(::cs_main);
    drivechain::Database& db{*m_blockman.m_drivechain_db};
    const Consensus::DrivechainParams& params{m_chainman.GetConsensus().drivechain};
    // The state of this chain as a sidechain goes with the blocks: without a store for it there is none.
    if (m_chainman.GetConsensus().sidechain.enabled && !side_store) {
        return util::Error{_("The sidechain state cannot be derived here. Restart with -reindex-chainstate.")};
    }
    // Brought forward for the chainstate itself: its store is written out as it goes.
    const bool own_cache{side_store && side_store == m_side_cache.get()};
    if (own_cache) m_side_changed_all = true;
    for (int height{from->nHeight + 1}; height <= to->nHeight; ++height) {
        const CBlockIndex* next{to->GetAncestor(height)};
        CBlock block;
        if (!m_blockman.ReadBlock(block, *next)) {
            return util::Error{strprintf(_("Failed to read block %s. Restart with -reindex."), next->GetBlockHash().ToString())};
        }
        drivechain::BlockUndo undo;
        std::vector<drivechain::Deposit> deposits;
        std::string reject_reason;
        CAmount minted{0};
        std::optional<sidechain::StoreOverlay> block_store;
        const std::optional<drivechain::SideContext> side{side_store ? m_chainman.SideContext(minted, block_store.emplace(*side_store, /*journal=*/true)) : std::nullopt};
        if (!scdb.ConnectBlock(block, height, params, undo, &deposits, reject_reason, side ? &*side : nullptr)) {
            LogError("%s: block %s breaks the drivechain rules (%s)", __func__, next->GetBlockHash().ToString(), reject_reason);
            // Back to the block before it (the undo data is complete at any point of failure); the
            // block's changes to the store are dropped with block_store.
            scdb.DisconnectBlock(undo);
            if (stopped_at && side && side->failed && !sidechain::RecordIndependent(reject_reason)) {
                *stopped_at = next;
                return {};
            }
            if (invalid) *invalid = next;
            return util::Error{strprintf(_("Block %s breaks the drivechain rules (%s). Restart with -reindex."), next->GetBlockHash().ToString(), reject_reason)};
        }
        db.WriteBlock(next->GetBlockHash(), height, undo, deposits, scdb, /*keep_undo=*/height > keep_undo_above);
        if (block_store) block_store->MergeInto(*side_store);
        // What the store holds in memory stays small: written out as it goes, with the database
        // it matches.
        if (own_cache && (height % 1000 == 0 || side_store->Bytes() > SIDE_CACHE_MAX_BYTES)) {
            m_scdb = scdb;
            WriteDrivechainState();
        }
        if (m_chainman.m_interrupt) return util::Error{_("Interrupted while loading the sidechain database.")};
    }
    return {};
}

VerifyDBResult CVerifyDB::VerifyDB(
    Chainstate& chainstate,
    const Consensus::Params& consensus_params,
    CCoinsView& coinsview,
    int nCheckLevel, int nCheckDepth)
{
    AssertLockHeld(cs_main);

    if (chainstate.m_chain.Tip() == nullptr || chainstate.m_chain.Tip()->pprev == nullptr) {
        return VerifyDBResult::SUCCESS;
    }

    // Verify blocks in the best chain
    if (nCheckDepth <= 0 || nCheckDepth > chainstate.m_chain.Height()) {
        nCheckDepth = chainstate.m_chain.Height();
    }
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogInfo("Verifying last %i blocks at level %i", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(&coinsview);
    drivechain::SidechainDB scdb{chainstate.m_scdb};
    // Blocks are undone and done again on an overlay of the sidechain state, dropped after.
    sidechain::StoreOverlay side_store{chainstate.SideCache(), /*journal=*/false};
    CBlockIndex* pindex;
    CBlockIndex* pindexFailure = nullptr;
    int nGoodTransactions = 0;
    BlockValidationState state;
    int reportDone = 0;
    bool skipped_no_block_data{false};
    bool skipped_l3_checks{false};
    LogInfo("Verification progress: 0%%");

    const bool is_snapshot_cs{chainstate.m_from_snapshot_blockhash};

    for (pindex = chainstate.m_chain.Tip(); pindex && pindex->pprev; pindex = pindex->pprev) {
        const int percentageDone = std::max(1, std::min(99, (int)(((double)(chainstate.m_chain.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));
        if (reportDone < percentageDone / 10) {
            // report every 10% step
            LogInfo("Verification progress: %d%%", percentageDone);
            reportDone = percentageDone / 10;
        }
        m_notifications.progress(_("Verifying blocks…"), percentageDone, false);
        if (pindex->nHeight <= chainstate.m_chain.Height() - nCheckDepth) {
            break;
        }
        if ((chainstate.m_blockman.IsPruneMode() || is_snapshot_cs) && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning or running under an assumeutxo snapshot, only go
            // back as far as we have data.
            LogInfo("Block verification stopping at height %d (no data). This could be due to pruning or use of an assumeutxo snapshot.", pindex->nHeight);
            skipped_no_block_data = true;
            break;
        }
        if (nCheckLevel >= 3 && !scdb.GetBlockHash().IsNull() && !chainstate.m_blockman.m_drivechain_db->HasBlockUndo(pindex->GetBlockHash())) {
            // Blocks deep in the chain no longer have the data to take back their drivechain changes:
            // as deep as a reorg can go is as deep as the check goes.
            LogInfo("Block verification stopping at height %d (drivechain undo data is kept for the last %d blocks).", pindex->nHeight, DRIVECHAIN_UNDO_DEPTH);
            break;
        }
        CBlock block;
        // check level 0: read from disk
        if (!chainstate.m_blockman.ReadBlock(block, *pindex)) {
            LogError("Verification error: ReadBlock failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            return VerifyDBResult::CORRUPTED_BLOCK_DB;
        }
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state, consensus_params)) {
            LogError("Verification error: found bad block at %d, hash=%s (%s)",
                      pindex->nHeight, pindex->GetBlockHash().ToString(), state.ToString());
            return VerifyDBResult::CORRUPTED_BLOCK_DB;
        }
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            if (!pindex->GetUndoPos().IsNull()) {
                if (!chainstate.m_blockman.ReadBlockUndo(undo, *pindex)) {
                    LogError("Verification error: found bad undo data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
                    return VerifyDBResult::CORRUPTED_BLOCK_DB;
                }
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        size_t curr_coins_usage = coins.DynamicMemoryUsage() + chainstate.CoinsTip().DynamicMemoryUsage() + side_store.Bytes();

        if (nCheckLevel >= 3) {
            if (curr_coins_usage <= chainstate.m_coinstip_cache_size_bytes) {
                assert(coins.GetBestBlock() == pindex->GetBlockHash());
                DisconnectResult res = chainstate.DisconnectBlock(block, pindex, coins, &scdb, &side_store);
                if (res == DISCONNECT_FAILED) {
                    LogError("Verification error: irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
                    return VerifyDBResult::CORRUPTED_BLOCK_DB;
                }
                if (res == DISCONNECT_UNCLEAN) {
                    nGoodTransactions = 0;
                    pindexFailure = pindex;
                } else {
                    nGoodTransactions += block.vtx.size();
                }
            } else {
                skipped_l3_checks = true;
            }
        }
        if (chainstate.m_chainman.m_interrupt) return VerifyDBResult::INTERRUPTED;
    }
    if (pindexFailure) {
        LogError("Verification error: coin database inconsistencies found (last %i blocks, %i good transactions before that)", chainstate.m_chain.Height() - pindexFailure->nHeight + 1, nGoodTransactions);
        return VerifyDBResult::CORRUPTED_BLOCK_DB;
    }
    if (skipped_l3_checks) {
        LogWarning("Skipped verification of level >=3 (insufficient database cache size). Consider increasing -dbcache.");
    }

    // store block count as we move pindex at check level >= 4
    int block_count = chainstate.m_chain.Height() - pindex->nHeight;

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4 && !skipped_l3_checks) {
        while (pindex != chainstate.m_chain.Tip()) {
            const int percentageDone = std::max(1, std::min(99, 100 - (int)(((double)(chainstate.m_chain.Height() - pindex->nHeight)) / (double)nCheckDepth * 50)));
            if (reportDone < percentageDone / 10) {
                // report every 10% step
                LogInfo("Verification progress: %d%%", percentageDone);
                reportDone = percentageDone / 10;
            }
            m_notifications.progress(_("Verifying blocks…"), percentageDone, false);
            pindex = chainstate.m_chain.Next(*pindex);
            CBlock block;
            if (!chainstate.m_blockman.ReadBlock(block, *pindex)) {
                LogError("Verification error: ReadBlock failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
                return VerifyDBResult::CORRUPTED_BLOCK_DB;
            }
            if (!chainstate.ConnectBlock(block, state, pindex, coins, /*fJustCheck=*/false, &scdb, &side_store)) {
                if (state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE) {
                    // A sidechain block that the record of the mainchain does not agree with any more
                    // (its commitment moved): the follower checks it again. The blocks after it cannot be.
                    LogWarning("Verification stopped at height %d, block %s: the record of the mainchain does not agree with it any more (%s)", pindex->nHeight, pindex->GetBlockHash().ToString(), state.ToString());
                    break;
                }
                LogError("Verification error: found unconnectable block at %d, hash=%s (%s)", pindex->nHeight, pindex->GetBlockHash().ToString(), state.ToString());
                return VerifyDBResult::CORRUPTED_BLOCK_DB;
            }
            if (chainstate.m_chainman.m_interrupt) return VerifyDBResult::INTERRUPTED;
        }
    }

    LogInfo("Verification: checked last %i blocks at level %i", block_count, nCheckLevel);
    if (nCheckLevel >= 3 && !skipped_l3_checks) {
        LogInfo("Verification: no coin database inconsistencies (%i transactions)", nGoodTransactions);
    }

    if (skipped_l3_checks) {
        return VerifyDBResult::SKIPPED_L3_CHECKS;
    }
    if (skipped_no_block_data) {
        return VerifyDBResult::SKIPPED_MISSING_BLOCKS;
    }
    return VerifyDBResult::SUCCESS;
}

/** Apply the effects of a block on the utxo cache, ignoring that it may already have been applied. */
bool Chainstate::RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs)
{
    AssertLockHeld(cs_main);
    // TODO: merge with ConnectBlock
    CBlock block;
    if (!m_blockman.ReadBlock(block, *pindex)) {
        LogError("ReplayBlock(): ReadBlock failed at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
        return false;
    }

    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const CTxIn &txin : tx->vin) {
                inputs.SpendCoin(txin.prevout);
            }
        }
        // Pass check = true as every addition may be an overwrite.
        AddCoins(inputs, *tx, pindex->nHeight, true);
    }
    return true;
}

bool Chainstate::ReplayBlocks()
{
    LOCK(cs_main);

    CCoinsView& db = this->CoinsDB();
    CCoinsViewCache cache(&db);

    std::vector<uint256> hashHeads = db.GetHeadBlocks();
    if (hashHeads.empty()) return true; // We're already in a consistent state.
    if (hashHeads.size() != 2) {
        LogError("ReplayBlocks(): unknown inconsistent state\n");
        return false;
    }

    m_chainman.GetNotifications().progress(_("Replaying blocks…"), 0, false);
    LogInfo("Replaying blocks");

    const CBlockIndex* pindexOld = nullptr;  // Old tip during the interrupted flush.
    const CBlockIndex* pindexNew;            // New tip during the interrupted flush.
    const CBlockIndex* pindexFork = nullptr; // Latest block common to both the old and the new tip.

    if (!m_blockman.m_block_index.contains(hashHeads[0])) {
        LogError("ReplayBlocks(): reorganization to unknown block requested\n");
        return false;
    }
    pindexNew = &(m_blockman.m_block_index[hashHeads[0]]);

    if (!hashHeads[1].IsNull()) { // The old tip is allowed to be 0, indicating it's the first flush.
        if (!m_blockman.m_block_index.contains(hashHeads[1])) {
            LogError("ReplayBlocks(): reorganization from unknown block requested\n");
            return false;
        }
        pindexOld = &(m_blockman.m_block_index[hashHeads[1]]);
        pindexFork = LastCommonAncestor(pindexOld, pindexNew);
        assert(pindexFork != nullptr);
    }

    // Rollback along the old branch.
    const int nForkHeight{pindexFork ? pindexFork->nHeight : 0};
    if (pindexOld != pindexFork) {
        LogInfo("Rolling back from %s (%i to %i)", pindexOld->GetBlockHash().ToString(), pindexOld->nHeight, nForkHeight);
        while (pindexOld != pindexFork) {
            if (pindexOld->nHeight > 0) { // Never disconnect the genesis block.
                CBlock block;
                if (!m_blockman.ReadBlock(block, *pindexOld)) {
                    LogError("RollbackBlock(): ReadBlock() failed at %d, hash=%s\n", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
                    return false;
                }
                if (pindexOld->nHeight % 10'000 == 0) {
                    LogInfo("Rolling back %s (%i)", pindexOld->GetBlockHash().ToString(), pindexOld->nHeight);
                }
                DisconnectResult res = DisconnectBlock(block, pindexOld, cache);
                if (res == DISCONNECT_FAILED) {
                    LogError("RollbackBlock(): DisconnectBlock failed at %d, hash=%s\n", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
                    return false;
                }
                // If DISCONNECT_UNCLEAN is returned, it means a non-existing UTXO was deleted, or an existing UTXO was
                // overwritten. It corresponds to cases where the block-to-be-disconnect never had all its operations
                // applied to the UTXO set. However, as both writing a UTXO and deleting a UTXO are idempotent operations,
                // the result is still a version of the UTXO set with the effects of that block undone.
            }
            pindexOld = pindexOld->pprev;
        }
        LogInfo("Rolled back to %s", pindexFork->GetBlockHash().ToString());
    }

    // Roll forward from the forking point to the new tip.
    if (nForkHeight < pindexNew->nHeight) {
        LogInfo("Rolling forward to %s (%i to %i)", pindexNew->GetBlockHash().ToString(), nForkHeight, pindexNew->nHeight);
        for (int nHeight = nForkHeight + 1; nHeight <= pindexNew->nHeight; ++nHeight) {
            const CBlockIndex& pindex{*Assert(pindexNew->GetAncestor(nHeight))};

            if (nHeight % 10'000 == 0) {
                LogInfo("Rolling forward %s (%i)", pindex.GetBlockHash().ToString(), nHeight);
            }
            m_chainman.GetNotifications().progress(_("Replaying blocks…"), (int)((nHeight - nForkHeight) * 100.0 / (pindexNew->nHeight - nForkHeight)), false);
            if (!RollforwardBlock(&pindex, cache)) return false;
        }
        LogInfo("Rolled forward to %s", pindexNew->GetBlockHash().ToString());
    }

    cache.SetBestBlock(pindexNew->GetBlockHash());
    cache.Flush(/*reallocate_cache=*/false); // local CCoinsViewCache goes out of scope
    m_chainman.GetNotifications().progress(bilingual_str{}, 100, false);
    return true;
}

bool Chainstate::NeedsRedownload() const
{
    AssertLockHeld(cs_main);

    // At and above m_params.SegwitHeight, segwit consensus rules must be validated
    CBlockIndex* block{m_chain.Tip()};

    while (block != nullptr && DeploymentActiveAt(*block, m_chainman, Consensus::DEPLOYMENT_SEGWIT)) {
        if (!(block->nStatus & BLOCK_OPT_WITNESS)) {
            // block is insufficiently validated for a segwit client
            return true;
        }
        block = block->pprev;
    }

    return false;
}

void Chainstate::ClearBlockIndexCandidates()
{
    AssertLockHeld(::cs_main);
    setBlockIndexCandidates.clear();
}

void Chainstate::PopulateBlockIndexCandidates()
{
    AssertLockHeld(::cs_main);

    for (CBlockIndex* pindex : m_blockman.GetAllBlockIndices()) {
        // With assumeutxo, the snapshot block is a candidate for the tip, but it
        // may not have BLOCK_VALID_TRANSACTIONS (e.g. if we haven't yet downloaded
        // the block), so we special-case it here.
        if (pindex == SnapshotBase() ||
                (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) &&
                 (pindex->HaveNumChainTxs() || pindex->pprev == nullptr))) {
            TryAddBlockIndexCandidate(pindex);
        }
    }
}

bool ChainstateManager::LoadBlockIndex()
{
    AssertLockHeld(cs_main);
    // Load block index from databases
    if (m_blockman.m_blockfiles_indexed) {
        bool ret{m_blockman.LoadBlockIndexDB(CurrentChainstate().m_from_snapshot_blockhash)};
        if (!ret) return false;

        m_blockman.ScanAndUnlinkAlreadyPrunedFiles();

        std::vector<CBlockIndex*> vSortedByHeight{m_blockman.GetAllBlockIndices()};
        std::sort(vSortedByHeight.begin(), vSortedByHeight.end(),
                  CBlockIndexHeightOnlyComparator());

        for (CBlockIndex* pindex : vSortedByHeight) {
            if (m_interrupt) return false;
            sidechain::Hooks().HeaderKept(*pindex);
            if (pindex->nStatus & BLOCK_FAILED_VALID && (!m_best_invalid || pindex->nChainWork > m_best_invalid->nChainWork)) {
                m_best_invalid = pindex;
            }
            if (pindex->IsValid(BLOCK_VALID_TREE) && (m_best_header == nullptr || CBlockIndexWorkComparator()(m_best_header, pindex)))
                m_best_header = pindex;
        }
    }
    return true;
}

bool ChainstateManager::LoadGenesisBlock()
{
    LOCK(cs_main);

    const CBlock& genesis_block{GetParams().GenesisBlock()};

    // Check whether we're already initialized by checking for genesis in
    // m_blockman.m_block_index. Note that we can't use a chainstate's m_chain here, since it is
    // set based on the coins db, not the block index db, which is the only
    // thing loaded at this point.
    if (m_blockman.m_block_index.contains(genesis_block.GetHash())) {
        return true;
    }

    try {
        FlatFilePos blockPos{m_blockman.WriteBlock(genesis_block, 0)};
        if (blockPos.IsNull()) {
            LogError("Writing genesis block to disk failed");
            return false;
        }
        CBlockIndex* pindex{m_blockman.AddToBlockIndex(genesis_block, m_best_header)};
        sidechain::Hooks().HeaderKept(*pindex);
        ReceivedBlockTransactions(genesis_block, pindex, blockPos);
    } catch (const std::runtime_error& e) {
        LogError("Failed to write genesis block: %s", e.what());
        return false;
    }

    return true;
}

void ChainstateManager::LoadExternalBlockFile(
    AutoFile& file_in,
    FlatFilePos* dbp,
    std::multimap<uint256, FlatFilePos>* blocks_with_unknown_parent)
{
    // Either both should be specified (-reindex), or neither (-loadblock).
    assert(!dbp == !blocks_with_unknown_parent);

    const auto start{SteadyClock::now()};
    const CChainParams& params{GetParams()};

    int nLoaded = 0;
    try {
        const uint32_t max_block_size{GetConsensus().MaxBlockSerializedSize()};
        BufferedFile blkdat{file_in, 2 * max_block_size, max_block_size + 8};
        // nRewind indicates where to resume scanning in case something goes wrong,
        // such as a block fails to deserialize.
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            if (m_interrupt) return;

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                MessageStartChars buf;
                blkdat.FindByte(std::byte(params.MessageStart()[0]));
                nRewind = blkdat.GetPos() + 1;
                blkdat >> buf;
                if (buf != params.MessageStart()) {
                    continue;
                }
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > max_block_size)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                // (this happens at the end of every blk.dat file)
                break;
            }
            try {
                // read block header
                const uint64_t nBlockPos{blkdat.GetPos()};
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                CBlockHeader header;
                blkdat >> header;
                const uint256 hash{header.GetHash()};
                // Skip the rest of this block (this may read from disk into memory); position to the marker before the
                // next block, but it's still possible to rewind to the start of the current block (without a disk read).
                nRewind = nBlockPos + nSize;
                blkdat.SkipTo(nRewind);

                std::shared_ptr<CBlock> pblock{}; // needs to remain available after the cs_main lock is released to avoid duplicate reads from disk

                {
                    LOCK(cs_main);
                    // detect out of order blocks, and store them for later
                    if (hash != params.GetConsensus().hashGenesisBlock && !m_blockman.LookupBlockIndex(header.hashPrevBlock)) {
                        LogDebug(BCLog::REINDEX, "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                                 header.hashPrevBlock.ToString());
                        if (dbp && blocks_with_unknown_parent) {
                            blocks_with_unknown_parent->emplace(header.hashPrevBlock, *dbp);
                        }
                        continue;
                    }

                    // process in case the block isn't known yet
                    const CBlockIndex* pindex = m_blockman.LookupBlockIndex(hash);
                    if (!pindex || (pindex->nStatus & BLOCK_HAVE_DATA) == 0) {
                        // This block can be processed immediately; rewind to its start, read and deserialize it.
                        blkdat.SetPos(nBlockPos);
                        pblock = std::make_shared<CBlock>();
                        blkdat >> TX_WITH_WITNESS(*pblock);
                        nRewind = blkdat.GetPos();

                        BlockValidationState state;
                        if (AcceptBlock(pblock, state, nullptr, true, dbp, nullptr, true)) {
                            nLoaded++;
                        }
                        if (state.IsError()) {
                            break;
                        }
                    } else if (hash != params.GetConsensus().hashGenesisBlock && pindex->nHeight % 1000 == 0) {
                        LogDebug(BCLog::REINDEX, "Block Import: already had block %s at height %d\n", hash.ToString(), pindex->nHeight);
                    }
                }

                // Activate the genesis block so normal node progress can continue
                // During first -reindex, this will only connect Genesis since
                // ActivateBestChain only connects blocks which are in the block tree db,
                // which only contains blocks whose parents are in it.
                // But do this only if genesis isn't activated yet, to avoid connecting many blocks
                // without assumevalid in the case of a continuation of a reindex that
                // was interrupted by the user.
                if (hash == params.GetConsensus().hashGenesisBlock && WITH_LOCK(::cs_main, return ActiveHeight()) == -1) {
                    BlockValidationState state;
                    if (!ActiveChainstate().ActivateBestChain(state, nullptr)) {
                        break;
                    }
                }

                if (m_blockman.IsPruneMode() && m_blockman.m_blockfiles_indexed && pblock) {
                    // must update the tip for pruning to work while importing with -loadblock.
                    // this is a tradeoff to conserve disk space at the expense of time
                    // spent updating the tip to be able to prune.
                    // otherwise, ActivateBestChain won't be called by the import process
                    // until after all of the block files are loaded. ActivateBestChain can be
                    // called by concurrent network message processing. but, that is not
                    // reliable for the purpose of pruning while importing.
                    if (auto result{ActivateBestChains()}; !result) {
                        LogDebug(BCLog::REINDEX, "%s\n", util::ErrorString(result).original);
                        break;
                    }
                }

                NotifyHeaderTip();

                if (!blocks_with_unknown_parent) continue;

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    auto range = blocks_with_unknown_parent->equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, FlatFilePos>::iterator it = range.first;
                        std::shared_ptr<CBlock> pblockrecursive = std::make_shared<CBlock>();
                        if (m_blockman.ReadBlock(*pblockrecursive, it->second, {})) {
                            const auto& block_hash{pblockrecursive->GetHash()};
                            LogDebug(BCLog::REINDEX, "%s: Processing out of order child %s of %s", __func__, block_hash.ToString(), head.ToString());
                            LOCK(cs_main);
                            BlockValidationState dummy;
                            if (AcceptBlock(pblockrecursive, dummy, nullptr, true, &it->second, nullptr, true)) {
                                nLoaded++;
                                queue.push_back(block_hash);
                            }
                        }
                        range.first++;
                        blocks_with_unknown_parent->erase(it);
                        NotifyHeaderTip();
                    }
                }
            } catch (const std::exception& e) {
                // historical bugs added extra data to the block files that does not deserialize cleanly.
                // commonly this data is between readable blocks, but it does not really matter. such data is not fatal to the import process.
                // the code that reads the block files deals with invalid data by simply ignoring it.
                // it continues to search for the next {4 byte magic message start bytes + 4 byte length + block} that does deserialize cleanly
                // and passes all of the other block validation checks dealing with POW and the merkle root, etc...
                // we merely note with this informational log message when unexpected data is encountered.
                // we could also be experiencing a storage system read error, or a read of a previous bad write. these are possible, but
                // less likely scenarios. we don't have enough information to tell a difference here.
                // the reindex process is not the place to attempt to clean and/or compact the block files. if so desired, a studious node operator
                // may use knowledge of the fact that the block files are not entirely pristine in order to prepare a set of pristine, and
                // perhaps ordered, block files for later reindexing.
                LogDebug(BCLog::REINDEX, "%s: unexpected data at file offset 0x%x - %s. continuing\n", __func__, (nRewind - 1), e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        GetNotifications().fatalError(strprintf(_("System error while loading external block file: %s"), e.what()));
    }
    LogInfo("Loaded %i blocks from external file in %dms", nLoaded, Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
}

bool ChainstateManager::ShouldCheckBlockIndex() const
{
    // Assert to verify Flatten() has been called.
    if (!*Assert(m_options.check_block_index)) return false;
    if (FastRandomContext().randrange(*m_options.check_block_index) >= 1) return false;
    return true;
}

void ChainstateManager::CheckBlockIndex() const
{
    if (!ShouldCheckBlockIndex()) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in m_blockman.m_block_index but no active chain. (A few of the
    // tests when iterating the block tree require that m_chain has been initialized.)
    if (ActiveChain().Height() < 0) {
        assert(m_blockman.m_block_index.size() <= 1);
        return;
    }

    CheckUnchecked();

    // Build forward-pointing data structure for the entire block tree.
    // For performance reasons, indexes of the best header chain are stored in a vector (within CChain).
    // All remaining blocks are stored in a multimap.
    // The best header chain can differ from the active chain: E.g. its entries may belong to blocks that
    // are not yet validated.
    CChain best_hdr_chain;
    assert(m_best_header);
    assert(!(m_best_header->nStatus & BLOCK_FAILED_VALID));
    best_hdr_chain.SetTip(*m_best_header);

    std::multimap<const CBlockIndex*, const CBlockIndex*> forward;
    for (auto& [_, block_index] : m_blockman.m_block_index) {
        // Only save indexes in forward that are not part of the best header chain.
        if (!best_hdr_chain.Contains(block_index)) {
            // Only genesis, which must be part of the best header chain, can have a nullptr parent.
            assert(block_index.pprev);
            forward.emplace(block_index.pprev, &block_index);
        }
    }
    assert(forward.size() + best_hdr_chain.Height() + 1 == m_blockman.m_block_index.size());

    const CBlockIndex* pindex = best_hdr_chain[0];
    assert(pindex);
    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    const CBlockIndex* pindexFirstInvalid = nullptr;              // Oldest ancestor of pindex which is invalid.
    const CBlockIndex* pindexFirstMissing = nullptr;              // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA, since assumeutxo snapshot if used.
    const CBlockIndex* pindexFirstNeverProcessed = nullptr;       // Oldest ancestor of pindex for which nTx == 0, since assumeutxo snapshot if used.
    const CBlockIndex* pindexFirstNotTreeValid = nullptr;         // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    const CBlockIndex* pindexFirstNotTransactionsValid = nullptr; // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS (regardless of being valid or not), since assumeutxo snapshot if used.
    const CBlockIndex* pindexFirstNotChainValid = nullptr;        // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not), since assumeutxo snapshot if used.
    const CBlockIndex* pindexFirstNotScriptsValid = nullptr;      // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not), since assumeutxo snapshot if used.

    // After checking an assumeutxo snapshot block, reset pindexFirst pointers
    // to earlier blocks that have not been downloaded or validated yet, so
    // checks for later blocks can assume the earlier blocks were validated and
    // be stricter, testing for more requirements.
    const CBlockIndex* snap_base{CurrentChainstate().SnapshotBase()};
    const CBlockIndex *snap_first_missing{}, *snap_first_notx{}, *snap_first_notv{}, *snap_first_nocv{}, *snap_first_nosv{};
    auto snap_update_firsts = [&] {
        if (pindex == snap_base) {
            std::swap(snap_first_missing, pindexFirstMissing);
            std::swap(snap_first_notx, pindexFirstNeverProcessed);
            std::swap(snap_first_notv, pindexFirstNotTransactionsValid);
            std::swap(snap_first_nocv, pindexFirstNotChainValid);
            std::swap(snap_first_nosv, pindexFirstNotScriptsValid);
        }
    };

    while (pindex != nullptr) {
        nNodes++;
        if (pindexFirstInvalid == nullptr && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == nullptr && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            pindexFirstMissing = pindex;
        }
        if (pindexFirstNeverProcessed == nullptr && pindex->nTx == 0) pindexFirstNeverProcessed = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotTreeValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;

        if (pindex->pprev != nullptr) {
            if (pindexFirstNotTransactionsValid == nullptr &&
                    (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) {
                pindexFirstNotTransactionsValid = pindex;
            }

            if (pindexFirstNotChainValid == nullptr &&
                    (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) {
                pindexFirstNotChainValid = pindex;
            }

            if (pindexFirstNotScriptsValid == nullptr &&
                    (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) {
                pindexFirstNotScriptsValid = pindex;
            }
        }

        // Begin: actual consistency checks.
        if (pindex->pprev == nullptr) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == GetConsensus().hashGenesisBlock); // Genesis block's hash must match.
            for (const auto& c : m_chainstates) {
                if (c->m_chain.Genesis() != nullptr) {
                    assert(pindex == c->m_chain.Genesis()); // The chain's genesis block must be this block.
                }
            }
        }
        // nSequenceId can't be set higher than SEQ_ID_INIT_FROM_DISK{1} for blocks that aren't linked
        // (negative is used for preciousblock, SEQ_ID_BEST_CHAIN_FROM_DISK{0} for active chain when loaded from disk)
        if (!pindex->HaveNumChainTxs()) assert(pindex->nSequenceId <= SEQ_ID_INIT_FROM_DISK);
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or not pruning has occurred).
        // HAVE_DATA is only equivalent to nTx > 0 (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!m_blockman.m_have_pruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx > 0
            assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else {
            // If we have pruned, then we can only say that HAVE_DATA implies nTx > 0
            if (pindex->nStatus & BLOCK_HAVE_DATA) assert(pindex->nTx > 0);
        }
        if (pindex->nStatus & BLOCK_HAVE_UNDO) assert(pindex->nStatus & BLOCK_HAVE_DATA);
        if (snap_base && snap_base->GetAncestor(pindex->nHeight) == pindex) {
            // Assumed-valid blocks should connect to the main chain.
            assert((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE);
        }
        // There should only be an nTx value if we have
        // actually seen a block's transactions.
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0)); // This is pruning-independent.
        // All parents having had data (at some point) is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to HaveNumChainTxs().
        // HaveNumChainTxs will also be set in the assumeutxo snapshot block from snapshot metadata.
        assert((pindexFirstNeverProcessed == nullptr || pindex == snap_base) == pindex->HaveNumChainTxs());
        assert((pindexFirstNotTransactionsValid == nullptr || pindex == snap_base) == pindex->HaveNumChainTxs());
        assert(pindex->nHeight == nHeight); // nHeight must be consistent.
        assert(pindex->pprev == nullptr || pindex->nChainWork >= pindex->pprev->nChainWork); // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight))); // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == nullptr); // All m_blockman.m_block_index entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == nullptr); // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == nullptr); // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == nullptr); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == nullptr) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_VALID) == 0); // The failed flag cannot be set for blocks without invalid parents.
        } else {
            assert(pindex->nStatus & BLOCK_FAILED_VALID); // Invalid blocks and their descendants must be marked as invalid
        }
        // Make sure m_chain_tx_count sum is correctly computed.
        if (!pindex->pprev) {
            // If no previous block, nTx and m_chain_tx_count must be the same.
            assert(pindex->m_chain_tx_count == pindex->nTx);
        } else if (pindex->pprev->m_chain_tx_count > 0 && pindex->nTx > 0) {
            // If previous m_chain_tx_count is set and number of transactions in block is known, sum must be set.
            assert(pindex->m_chain_tx_count == pindex->nTx + pindex->pprev->m_chain_tx_count);
        } else {
            // Otherwise m_chain_tx_count should only be set if this is a snapshot
            // block, and must be set if it is.
            assert((pindex->m_chain_tx_count != 0) == (pindex == snap_base));
        }
        // There should be no block with more work than m_best_header, unless it's known to be invalid
        assert((pindex->nStatus & BLOCK_FAILED_VALID) || pindex->nChainWork <= m_best_header->nChainWork);

        // Chainstate-specific checks on setBlockIndexCandidates
        for (const auto& c : m_chainstates) {
            if (c->m_chain.Tip() == nullptr) continue;
            // Two main factors determine whether pindex is a candidate in
            // setBlockIndexCandidates:
            //
            // - If pindex has less work than the chain tip, it should not be a
            //   candidate, and this will be asserted below. Otherwise it is a
            //   potential candidate.
            //
            // - If pindex or one of its parent blocks back to the genesis block
            //   or an assumeutxo snapshot never downloaded transactions
            //   (pindexFirstNeverProcessed is non-null), it should not be a
            //   candidate, and this will be asserted below. The only exception
            //   is if pindex itself is an assumeutxo snapshot block. Then it is
            //   also a potential candidate.
            if (!CBlockIndexWorkComparator()(pindex, c->m_chain.Tip()) && (pindexFirstNeverProcessed == nullptr || pindex == snap_base)) {
                // If pindex was detected as invalid (pindexFirstInvalid is
                // non-null), it is not required to be in
                // setBlockIndexCandidates.
                if (pindexFirstInvalid == nullptr) {
                    // If pindex and all its parents back to the genesis block
                    // or an assumeutxo snapshot block downloaded transactions,
                    // and the transactions were not pruned (pindexFirstMissing
                    // is null), it is a potential candidate. The check
                    // excludes pruned blocks, because if any blocks were
                    // pruned between pindex and the current chain tip, pindex will
                    // only temporarily be added to setBlockIndexCandidates,
                    // before being moved to m_blocks_unlinked. This check
                    // could be improved to verify that if all blocks between
                    // the chain tip and pindex have data, pindex must be a
                    // candidate.
                    //
                    // If pindex is the chain tip, it also is a potential
                    // candidate.
                    //
                    // If the chainstate was loaded from a snapshot and pindex
                    // is the base of the snapshot, pindex is also a potential
                    // candidate.
                    if (pindexFirstMissing == nullptr || pindex == c->m_chain.Tip() || pindex == c->SnapshotBase()) {
                        // If this chainstate is not a historical chainstate
                        // targeting a specific block, pindex must be in
                        // setBlockIndexCandidates. Otherwise, pindex only
                        // needs to be added if it is an ancestor of the target
                        // block.
                        if (!c->TargetBlock() || c->TargetBlock()->GetAncestor(pindex->nHeight) == pindex) {
                            assert(c->setBlockIndexCandidates.contains(pindex));
                        }
                    }
                    // If some parent is missing, then it could be that this block was in
                    // setBlockIndexCandidates but had to be removed because of the missing data.
                    // In this case it must be in m_blocks_unlinked -- see test below.
                }
            } else { // If this block sorts worse than the current tip or some ancestor's block has never been seen, it cannot be in setBlockIndexCandidates.
                assert(!c->setBlockIndexCandidates.contains(pindex));
            }
        }
        // Check whether this block is in m_blocks_unlinked.
        auto rangeUnlinked{m_blockman.m_blocks_unlinked.equal_range(pindex->pprev)};
        bool foundInUnlinked = false;
        for (auto it = rangeUnlinked.first; it != rangeUnlinked.second; ++it) {
            assert(it->first == pindex->pprev);
            if (it->second == pindex) {
                assert(!foundInUnlinked); // No duplicates in m_blocks_unlinked
                foundInUnlinked = true;
            }
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed != nullptr && pindexFirstInvalid == nullptr) {
            // If this block has block data available, some parent was never received, and has no invalid parents, it must be in m_blocks_unlinked.
            assert(foundInUnlinked);
        }
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) assert(!foundInUnlinked); // Can't be in m_blocks_unlinked if we don't HAVE_DATA
        if (pindexFirstMissing == nullptr) assert(!foundInUnlinked); // We aren't missing data for any parent -- cannot be in m_blocks_unlinked.
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed == nullptr && pindexFirstMissing != nullptr) {
            // We HAVE_DATA for this block, have received data for all parents at some point, but we're currently missing data for some parent.
            assert(m_blockman.m_have_pruned);
            // This block may have entered m_blocks_unlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between m_chain and the
            //    tip.
            // So if this block is itself better than any m_chain.Tip() and it wasn't in
            // setBlockIndexCandidates, then it must be in m_blocks_unlinked.
            for (const auto& c : m_chainstates) {
                if (!CBlockIndexWorkComparator()(pindex, c->m_chain.Tip()) && !c->setBlockIndexCandidates.contains(pindex)) {
                    if (pindexFirstInvalid == nullptr) {
                        if (!c->TargetBlock() || c->TargetBlock()->GetAncestor(pindex->nHeight) == pindex) {
                            assert(foundInUnlinked);
                        }
                    }
                }
            }
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.


        // Try descending into the first subnode. Always process forks first and the best header chain after.
        snap_update_firsts();
        auto range{forward.equal_range(pindex)};
        if (range.first != range.second) {
            // A subnode not part of the best header chain was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        } else if (best_hdr_chain.Contains(*pindex)) {
            // Descend further into best header chain.
            nHeight++;
            pindex = best_hdr_chain[nHeight];
            if (!pindex) break; // we are finished, since the best header chain is always processed last
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            snap_update_firsts();
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = nullptr;
            if (pindex == pindexFirstMissing) pindexFirstMissing = nullptr;
            if (pindex == pindexFirstNeverProcessed) pindexFirstNeverProcessed = nullptr;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = nullptr;
            if (pindex == pindexFirstNotTransactionsValid) pindexFirstNotTransactionsValid = nullptr;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = nullptr;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = nullptr;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            auto rangePar{forward.equal_range(pindexPar)};
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to a sibling not part of the best header chain.
                pindex = rangePar.first->second;
                break;
            } else if (pindexPar == best_hdr_chain[nHeight - 1]) {
                // Move to pindex's sibling on the best-chain, if it has one.
                pindex = best_hdr_chain[nHeight];
                // There will not be a next block if (and only if) parent block is the best header.
                assert((pindex == nullptr) == (pindexPar == best_hdr_chain.Tip()));
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire block index.
    assert(nNodes == forward.size() + best_hdr_chain.Height() + 1);
}

std::string Chainstate::ToString()
{
    AssertLockHeld(::cs_main);
    CBlockIndex* tip = m_chain.Tip();
    return strprintf("Chainstate [%s] @ height %d (%s)",
                     m_from_snapshot_blockhash ? "snapshot" : "ibd",
                     tip ? tip->nHeight : -1, tip ? tip->GetBlockHash().ToString() : "null");
}

bool Chainstate::ResizeCoinsCaches(size_t coinstip_size, size_t coinsdb_size)
{
    AssertLockHeld(::cs_main);
    if (coinstip_size == m_coinstip_cache_size_bytes &&
            coinsdb_size == m_coinsdb_cache_size_bytes) {
        // Cache sizes are unchanged, no need to continue.
        return true;
    }
    size_t old_coinstip_size = m_coinstip_cache_size_bytes;
    m_coinstip_cache_size_bytes = coinstip_size;
    m_coinsdb_cache_size_bytes = coinsdb_size;
    CoinsDB().ResizeCache(coinsdb_size);

    LogInfo("[%s] resized coinsdb cache to %.1f MiB",
        this->ToString(), coinsdb_size / double(1_MiB));
    LogInfo("[%s] resized coinstip cache to %.1f MiB",
        this->ToString(), coinstip_size / double(1_MiB));

    BlockValidationState state;
    bool ret;

    if (coinstip_size > old_coinstip_size) {
        // Likely no need to flush if cache sizes have grown.
        ret = FlushStateToDisk(state, FlushStateMode::IF_NEEDED);
    } else {
        // Otherwise, flush state to disk and deallocate the in-memory coins map.
        ret = FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH);
    }
    return ret;
}

double ChainstateManager::GuessVerificationProgress(const CBlockIndex* pindex) const
{
    AssertLockHeld(GetMutex());
    const ChainTxData& data{GetParams().TxData()};
    if (pindex == nullptr) {
        return 0.0;
    }

    if (pindex->m_chain_tx_count == 0) {
        LogDebug(BCLog::VALIDATION, "Block %d has unset m_chain_tx_count. Unable to estimate verification progress.\n", pindex->nHeight);
        return 0.0;
    }

    const int64_t nNow{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
    // With a chain order of a sidechain's own, the header of the most work may be lower than the tip
    // (a sidechain's key need not grow with the height alone): not a bug then.
    const auto best_header_above{[&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return sidechain::Hooks().CustomChainOrder() ? m_best_header->nHeight >= pindex->nHeight : Assume(m_best_header->nHeight >= pindex->nHeight);
    }};
    const auto block_time{
        (Assume(m_best_header) && std::abs(nNow - pindex->GetBlockTime()) <= Ticks<std::chrono::seconds>(2h) &&
         best_header_above()) ?
            // When the header is known to be recent, switch to a height-based
            // approach. This ensures the returned value is quantized when
            // close to "1.0", because some users expect it to be. This also
            // avoids relying too much on the exact miner-set timestamp, which
            // may be off.
            nNow - (m_best_header->nHeight - pindex->nHeight) * GetConsensus().nPowTargetSpacing :
            pindex->GetBlockTime(),
    };

    double fTxTotal;

    if (pindex->m_chain_tx_count <= data.tx_count) {
        fTxTotal = data.tx_count + (nNow - data.nTime) * data.dTxRate;
    } else {
        fTxTotal = pindex->m_chain_tx_count + (nNow - block_time) * data.dTxRate;
    }

    return std::min<double>(pindex->m_chain_tx_count / fTxTotal, 1.0);
}

double ChainstateManager::GetBackgroundVerificationProgress(const CBlockIndex& pindex) const
{
    AssertLockHeld(GetMutex());
    Assert(HistoricalChainstate());
    auto target_block = HistoricalChainstate()->TargetBlock();

    if (pindex.m_chain_tx_count == 0 || target_block->m_chain_tx_count == 0) {
        LogDebug(BCLog::VALIDATION, "[background validation] Block %d has unset m_chain_tx_count. Unable to estimate verification progress.", pindex.nHeight);
        return 0.0;
    }
    return static_cast<double>(pindex.m_chain_tx_count) / static_cast<double>(target_block->m_chain_tx_count);
}

Chainstate& ChainstateManager::InitializeChainstate(CTxMemPool* mempool)
{
    AssertLockHeld(::cs_main);
    assert(m_chainstates.empty());
    m_chainstates.emplace_back(std::make_unique<Chainstate>(mempool, m_blockman, *this));
    return *m_chainstates.back();
}

[[nodiscard]] static bool DeleteCoinsDBFromDisk(const fs::path db_path, bool is_snapshot)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);

    if (is_snapshot) {
        fs::path base_blockhash_path = db_path / node::SNAPSHOT_BLOCKHASH_FILENAME;

        try {
            bool existed = fs::remove(base_blockhash_path);
            if (!existed) {
                LogWarning("[snapshot] snapshot chainstate dir being removed lacks %s file",
                          fs::PathToString(node::SNAPSHOT_BLOCKHASH_FILENAME));
            }
        } catch (const fs::filesystem_error& e) {
            LogWarning("[snapshot] failed to remove file %s: %s\n",
                       fs::PathToString(base_blockhash_path), e.code().message());
        }
    }

    std::string path_str = fs::PathToString(db_path);
    LogInfo("Removing leveldb dir at %s\n", path_str);

    // We have to destruct before this call leveldb::DB in order to release the db
    // lock, otherwise `DestroyDB` will fail. See `leveldb::~DBImpl()`.
    const bool destroyed = DestroyDB(path_str);

    if (!destroyed) {
        LogError("leveldb DestroyDB call failed on %s", path_str);
    }

    // Datadir should be removed from filesystem; otherwise initialization may detect
    // it on subsequent statups and get confused.
    //
    // If the base_blockhash_path removal above fails in the case of snapshot
    // chainstates, this will return false since leveldb won't remove a non-empty
    // directory.
    return destroyed && !fs::exists(db_path);
}

util::Result<CBlockIndex*> ChainstateManager::ActivateSnapshot(
        AutoFile& coins_file,
        const SnapshotMetadata& metadata,
        bool in_memory)
{
    uint256 base_blockhash = metadata.m_base_blockhash;

    CBlockIndex* snapshot_start_block{};

    {
        LOCK(::cs_main);

        if (this->CurrentChainstate().m_from_snapshot_blockhash) {
            return util::Error{Untranslated("Can't activate a snapshot-based chainstate more than once")};
        }
        // zSide: a snapshot has the coins, not the shielded pool (nor the rest of the sidechain
        // state), which would start empty: every note made before it unspendable, the pool's
        // balance wrong. Only regtest has snapshots (MakeSidechain clears them); not as a sidechain.
        if (GetConsensus().sidechain.enabled) {
            return util::Error{Untranslated("A sidechain cannot start from a UTXO snapshot: its state (the shielded pool) is not in it")};
        }
        // A UTXO snapshot does not hold the drivechain state (sidechains, escrows, bundles): the
        // chainstate made from it would start with an empty sidechain database and judge the blocks
        // after the snapshot by it. Only regtest, whose upstream tests use snapshots at heights
        // before any sidechain, takes them.
        if (GetConsensus().drivechain.max_sidechains > 0 && GetParams().GetChainType() != ChainType::REGTEST) {
            return util::Error{Untranslated("UTXO snapshots are not supported with drivechains: the snapshot does not hold the drivechain state")};
        }
        if (!GetParams().AssumeutxoForBlockhash(base_blockhash).has_value()) {
            auto available_heights = GetParams().GetAvailableSnapshotHeights();
            std::string heights_formatted = util::Join(available_heights, ", ", [&](const auto& i) { return util::ToString(i); });
            return util::Error{Untranslated(strprintf("assumeutxo block hash in snapshot metadata not recognized (hash: %s). The following snapshot heights are available: %s",
                base_blockhash.ToString(),
                heights_formatted))};
        }

        snapshot_start_block = m_blockman.LookupBlockIndex(base_blockhash);
        if (!snapshot_start_block) {
            return util::Error{Untranslated(strprintf("The base block header (%s) must appear in the headers chain. Make sure all headers are syncing, and call loadtxoutset again",
                          base_blockhash.ToString()))};
        }

        bool start_block_invalid = snapshot_start_block->nStatus & BLOCK_FAILED_VALID;
        if (start_block_invalid) {
            return util::Error{Untranslated(strprintf("The base block header (%s) is part of an invalid chain", base_blockhash.ToString()))};
        }

        if (!m_best_header || m_best_header->GetAncestor(snapshot_start_block->nHeight) != snapshot_start_block) {
            return util::Error{Untranslated("A forked headers-chain with more work than the chain with the snapshot base block header exists. Please proceed to sync without AssumeUtxo.")};
        }

        auto mempool{CurrentChainstate().GetMempool()};
        if (mempool && mempool->size() > 0) {
            return util::Error{Untranslated("Can't activate a snapshot when mempool not empty")};
        }
    }

    int64_t current_coinsdb_cache_size{0};
    int64_t current_coinstip_cache_size{0};

    // Cache percentages to allocate to each chainstate.
    //
    // These particular percentages don't matter so much since they will only be
    // relevant during snapshot activation; caches are rebalanced at the conclusion of
    // this function. We want to give (essentially) all available cache capacity to the
    // snapshot to aid the bulk load later in this function.
    static constexpr double IBD_CACHE_PERC = 0.01;
    static constexpr double SNAPSHOT_CACHE_PERC = 0.99;

    {
        LOCK(::cs_main);
        // Resize the coins caches to ensure we're not exceeding memory limits.
        //
        // Allocate the majority of the cache to the incoming snapshot chainstate, since
        // (optimistically) getting to its tip will be the top priority. We'll need to call
        // `MaybeRebalanceCaches()` once we're done with this function to ensure
        // the right allocation (including the possibility that no snapshot was activated
        // and that we should restore the active chainstate caches to their original size).
        //
        current_coinsdb_cache_size = this->ActiveChainstate().m_coinsdb_cache_size_bytes;
        current_coinstip_cache_size = this->ActiveChainstate().m_coinstip_cache_size_bytes;

        // Temporarily resize the active coins cache to make room for the newly-created
        // snapshot chain.
        this->ActiveChainstate().ResizeCoinsCaches(
            static_cast<size_t>(current_coinstip_cache_size * IBD_CACHE_PERC),
            static_cast<size_t>(current_coinsdb_cache_size * IBD_CACHE_PERC));
    }

    auto snapshot_chainstate = WITH_LOCK(::cs_main,
        return std::make_unique<Chainstate>(
            /*mempool=*/nullptr, m_blockman, *this, base_blockhash));

    {
        LOCK(::cs_main);
        snapshot_chainstate->InitCoinsDB(
            static_cast<size_t>(current_coinsdb_cache_size * SNAPSHOT_CACHE_PERC),
            in_memory, /*should_wipe=*/false);
        snapshot_chainstate->InitCoinsCache(
            static_cast<size_t>(current_coinstip_cache_size * SNAPSHOT_CACHE_PERC));
    }

    auto cleanup_bad_snapshot = [&](bilingual_str reason) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        this->MaybeRebalanceCaches();

        // PopulateAndValidateSnapshot can return (in error) before the leveldb datadir
        // has been created, so only attempt removal if we got that far.
        if (auto snapshot_datadir = node::FindAssumeutxoChainstateDir(m_options.datadir)) {
            // We have to destruct leveldb::DB in order to release the db lock, otherwise
            // DestroyDB() (in DeleteCoinsDBFromDisk()) will fail. See `leveldb::~DBImpl()`.
            // Destructing the chainstate (and so resetting the coinsviews object) does this.
            snapshot_chainstate.reset();
            bool removed = DeleteCoinsDBFromDisk(*snapshot_datadir, /*is_snapshot=*/true);
            if (!removed) {
                GetNotifications().fatalError(strprintf(_("Failed to remove snapshot chainstate dir (%s). "
                    "Manually remove it before restarting.\n"), fs::PathToString(*snapshot_datadir)));
            }
        }
        return util::Error{std::move(reason)};
    };

    if (auto res{this->PopulateAndValidateSnapshot(*snapshot_chainstate, coins_file, metadata)}; !res) {
        LOCK(::cs_main);
        return cleanup_bad_snapshot(Untranslated(strprintf("Population failed: %s", util::ErrorString(res).original)));
    }

    LOCK(::cs_main);  // cs_main required for rest of snapshot activation.

    // Do a final check to ensure that the snapshot chainstate is actually a more
    // work chain than the active chainstate; a user could have loaded a snapshot
    // very late in the IBD process, and we wouldn't want to load a useless chainstate.
    if (!CBlockIndexWorkComparator()(ActiveTip(), snapshot_chainstate->m_chain.Tip())) {
        return cleanup_bad_snapshot(Untranslated("work does not exceed active chainstate"));
    }
    // If not in-memory, persist the base blockhash for use during subsequent
    // initialization.
    if (!in_memory) {
        if (!node::WriteSnapshotBaseBlockhash(*snapshot_chainstate)) {
            return cleanup_bad_snapshot(Untranslated("could not write base blockhash"));
        }
    }

    // Whatever an earlier attempt with the same snapshot left of a sidechain state goes: the chainstate starts without one.
    if (auto reset{snapshot_chainstate->ResetDrivechainState()}; !reset) {
        return cleanup_bad_snapshot(util::ErrorString(reset));
    }
    Chainstate& chainstate{AddChainstate(std::move(snapshot_chainstate))};
    m_blockman.m_snapshot_height = Assert(chainstate.SnapshotBase())->nHeight;

    chainstate.PopulateBlockIndexCandidates();

    LogInfo("[snapshot] successfully activated snapshot %s", base_blockhash.ToString());
    LogInfo("[snapshot] (%.2f MB)",
              chainstate.CoinsTip().DynamicMemoryUsage() / (1000 * 1000));

    this->MaybeRebalanceCaches();
    return snapshot_start_block;
}

static void FlushSnapshotToDisk(CCoinsViewCache& coins_cache, bool snapshot_loaded)
{
    LOG_TIME_MILLIS_WITH_CATEGORY_MSG_ONCE(
        strprintf("%s (%.2f MB)",
                  snapshot_loaded ? "saving snapshot chainstate" : "flushing coins cache",
                  coins_cache.DynamicMemoryUsage() / (1000 * 1000)),
        BCLog::LogFlags::ALL);

    coins_cache.Flush();
}

struct StopHashingException : public std::exception
{
    const char* what() const noexcept override
    {
        return "ComputeUTXOStats interrupted.";
    }
};

static void SnapshotUTXOHashBreakpoint(const util::SignalInterrupt& interrupt)
{
    if (interrupt) throw StopHashingException();
}

util::Result<void> ChainstateManager::PopulateAndValidateSnapshot(
    Chainstate& snapshot_chainstate,
    AutoFile& coins_file,
    const SnapshotMetadata& metadata)
{
    // It's okay to release cs_main before we're done using `coins_cache` because we know
    // that nothing else will be referencing the newly created snapshot_chainstate yet.
    CCoinsViewCache& coins_cache = *WITH_LOCK(::cs_main, return &snapshot_chainstate.CoinsTip());

    uint256 base_blockhash = metadata.m_base_blockhash;

    CBlockIndex* snapshot_start_block = WITH_LOCK(::cs_main, return m_blockman.LookupBlockIndex(base_blockhash));

    if (!snapshot_start_block) {
        // Needed for ComputeUTXOStats to determine the
        // height and to avoid a crash when base_blockhash.IsNull()
        return util::Error{Untranslated(strprintf("Did not find snapshot start blockheader %s",
                  base_blockhash.ToString()))};
    }

    int base_height = snapshot_start_block->nHeight;
    const auto& maybe_au_data = GetParams().AssumeutxoForHeight(base_height);

    if (!maybe_au_data) {
        return util::Error{Untranslated(strprintf("Assumeutxo height in snapshot metadata not recognized "
                  "(%d) - refusing to load snapshot", base_height))};
    }

    const AssumeutxoData& au_data = *maybe_au_data;

    // This work comparison is a duplicate check with the one performed later in
    // ActivateSnapshot(), but is done so that we avoid doing the long work of staging
    // a snapshot that isn't actually usable.
    if (WITH_LOCK(::cs_main, return !CBlockIndexWorkComparator()(ActiveTip(), snapshot_start_block))) {
        return util::Error{Untranslated("Work does not exceed active chainstate")};
    }

    const uint64_t coins_count = metadata.m_coins_count;
    uint64_t coins_left = metadata.m_coins_count;

    LogInfo("[snapshot] loading %d coins from snapshot %s", coins_left, base_blockhash.ToString());
    int64_t coins_processed{0};

    while (coins_left > 0) {
        try {
            Txid txid;
            coins_file >> txid;
            size_t coins_per_txid{0};
            coins_per_txid = ReadCompactSize(coins_file);

            if (coins_per_txid > coins_left) {
                return util::Error{Untranslated("Mismatch in coins count in snapshot metadata and actual snapshot data")};
            }

            for (size_t i = 0; i < coins_per_txid; i++) {
                COutPoint outpoint;
                Coin coin;
                outpoint.n = static_cast<uint32_t>(ReadCompactSize(coins_file));
                outpoint.hash = txid;
                coins_file >> coin;
                if (coin.nHeight > base_height ||
                    outpoint.n >= std::numeric_limits<decltype(outpoint.n)>::max() // Avoid integer wrap-around in coinstats.cpp:ApplyHash
                ) {
                    return util::Error{Untranslated(strprintf("Bad snapshot data after deserializing %d coins",
                              coins_count - coins_left))};
                }
                if (!MoneyRange(coin.out.nValue)) {
                    return util::Error{Untranslated(strprintf("Bad snapshot data after deserializing %d coins - bad tx out value",
                              coins_count - coins_left))};
                }
                coins_cache.EmplaceCoinInternalDANGER(outpoint, std::move(coin));

                --coins_left;
                ++coins_processed;

                if (coins_processed % 1000000 == 0) {
                    LogInfo("[snapshot] %d coins loaded (%.2f%%, %.2f MB)",
                        coins_processed,
                        static_cast<float>(coins_processed) * 100 / static_cast<float>(coins_count),
                        coins_cache.DynamicMemoryUsage() / (1000 * 1000));
                }

                // Batch write and flush (if we need to) every so often.
                //
                // If our average Coin size is roughly 41 bytes, checking every 120,000 coins
                // means <5MB of memory imprecision.
                if (coins_processed % 120000 == 0) {
                    if (m_interrupt) {
                        return util::Error{Untranslated("Aborting after an interrupt was requested")};
                    }

                    const auto snapshot_cache_state = WITH_LOCK(::cs_main,
                        return snapshot_chainstate.GetCoinsCacheSizeState());

                    if (snapshot_cache_state >= CoinsCacheSizeState::CRITICAL) {
                        // This is a hack - we don't know what the actual best block is, but that
                        // doesn't matter for the purposes of flushing the cache here. We'll set this
                        // to its correct value (`base_blockhash`) below after the coins are loaded.
                        coins_cache.SetBestBlock(GetRandHash());

                        // No need to acquire cs_main since this chainstate isn't being used yet.
                        FlushSnapshotToDisk(coins_cache, /*snapshot_loaded=*/false);
                    }
                }
            }
        } catch (const std::ios_base::failure&) {
            return util::Error{Untranslated(strprintf("Bad snapshot format or truncated snapshot after deserializing %d coins",
                      coins_processed))};
        }
    }

    // Important that we set this. This and the coins_cache accesses above are
    // sort of a layer violation, but either we reach into the innards of
    // CCoinsViewCache here or we have to invert some of the Chainstate to
    // embed them in a snapshot-activation-specific CCoinsViewCache bulk load
    // method.
    coins_cache.SetBestBlock(base_blockhash);

    bool out_of_coins{false};
    try {
        std::byte left_over_byte;
        coins_file >> left_over_byte;
    } catch (const std::ios_base::failure&) {
        // We expect an exception since we should be out of coins.
        out_of_coins = true;
    }
    if (!out_of_coins) {
        return util::Error{Untranslated(strprintf("Bad snapshot - coins left over after deserializing %d coins",
            coins_count))};
    }

    LogInfo("[snapshot] loaded %d (%.2f MB) coins from snapshot %s",
        coins_count,
        coins_cache.DynamicMemoryUsage() / (1000 * 1000),
        base_blockhash.ToString());

    // No need to acquire cs_main since this chainstate isn't being used yet.
    FlushSnapshotToDisk(coins_cache, /*snapshot_loaded=*/true);

    assert(coins_cache.GetBestBlock() == base_blockhash);

    // As above, okay to immediately release cs_main here since no other context knows
    // about the snapshot_chainstate.
    const CCoinsViewDB& snapshot_coinsdb = WITH_LOCK(::cs_main, return snapshot_chainstate.CoinsDB());

    std::optional<CCoinsStats> maybe_stats;

    try {
        maybe_stats = ComputeUTXOStats(
            CoinStatsHashType::HASH_SERIALIZED, snapshot_coinsdb, m_blockman, [&interrupt = m_interrupt] { SnapshotUTXOHashBreakpoint(interrupt); });
    } catch (StopHashingException const&) {
        return util::Error{Untranslated("Aborting after an interrupt was requested")};
    }
    if (!maybe_stats.has_value()) {
        return util::Error{Untranslated("Failed to generate coins stats")};
    }

    // Assert that the deserialized chainstate contents match the expected assumeutxo value.
    if (AssumeutxoHash{maybe_stats->hashSerialized} != au_data.hash_serialized) {
        return util::Error{Untranslated(strprintf("Bad snapshot content hash: expected %s, got %s",
            au_data.hash_serialized.ToString(), maybe_stats->hashSerialized.ToString()))};
    }

    snapshot_chainstate.m_chain.SetTip(*snapshot_start_block);

    // The remainder of this function requires modifying data protected by cs_main.
    LOCK(::cs_main);

    // Fake various pieces of CBlockIndex state:
    CBlockIndex* index = nullptr;

    // Don't make any modifications to the genesis block since it shouldn't be
    // necessary, and since the genesis block doesn't have normal flags like
    // BLOCK_VALID_SCRIPTS set.
    constexpr int AFTER_GENESIS_START{1};

    for (int i = AFTER_GENESIS_START; i <= snapshot_chainstate.m_chain.Height(); ++i) {
        index = snapshot_chainstate.m_chain[i];

        // Fake BLOCK_OPT_WITNESS so that Chainstate::NeedsRedownload()
        // won't ask for -reindex on startup.
        if (DeploymentActiveAt(*index, *this, Consensus::DEPLOYMENT_SEGWIT)) {
            index->nStatus |= BLOCK_OPT_WITNESS;
        }

        m_blockman.m_dirty_blockindex.insert(index);
        // Changes to the block index will be flushed to disk after this call
        // returns in `ActivateSnapshot()`, when `MaybeRebalanceCaches()` is
        // called, since we've added a snapshot chainstate and therefore will
        // have to downsize the IBD chainstate, which will result in a call to
        // `FlushStateToDisk(FORCE_FLUSH)`.
    }

    assert(index);
    assert(index == snapshot_start_block);
    index->m_chain_tx_count = au_data.m_chain_tx_count;

    LogInfo("[snapshot] validated snapshot (%.2f MB)",
        coins_cache.DynamicMemoryUsage() / (1000 * 1000));
    return {};
}

// Currently, this function holds cs_main for its duration, which could be for
// multiple minutes due to the ComputeUTXOStats call. Holding cs_main used to be
// necessary (before d43a1f1a2fa3) to avoid advancing validated_cs farther than
// its target block. Now it should be possible to avoid this, but simply
// releasing cs_main here would not be possible because this function is invoked
// by ConnectTip within ActivateBestChain.
//
// Eventually (TODO) it would be better to call this function outside of
// ActivateBestChain, on a separate thread that should not require cs_main to
// hash, because the UTXO set is only hashed after the historical chainstate
// reaches its target block and is no longer changing.
SnapshotCompletionResult ChainstateManager::MaybeValidateSnapshot(Chainstate& validated_cs, Chainstate& unvalidated_cs)
{
    AssertLockHeld(cs_main);

    // If the snapshot does not need to be validated...
    if (unvalidated_cs.m_assumeutxo != Assumeutxo::UNVALIDATED ||
            // Or if either chainstate is unusable...
            !unvalidated_cs.m_from_snapshot_blockhash ||
            validated_cs.m_assumeutxo != Assumeutxo::VALIDATED ||
            !validated_cs.m_chain.Tip() ||
            // Or the validated chainstate is not targeting the snapshot block...
            !validated_cs.TargetBlockHash() ||
            *validated_cs.TargetBlockHash() != *unvalidated_cs.m_from_snapshot_blockhash ||
            // Or the validated chainstate has not reached the snapshot block yet...
            !validated_cs.ReachedTarget()) {
       // Then the snapshot cannot be validated and there is nothing to do.
       return SnapshotCompletionResult::SKIPPED;
    }
    assert(validated_cs.TargetBlock() == validated_cs.m_chain.Tip());

    auto handle_invalid_snapshot = [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        bilingual_str user_error = strprintf(_(
            "%s failed to validate the -assumeutxo snapshot state. "
            "This indicates a hardware problem, or a bug in the software, or a "
            "bad software modification that allowed an invalid snapshot to be "
            "loaded. As a result of this, the node will shut down and stop using any "
            "state that was built on the snapshot, resetting the chain height "
            "from %d to %d. On the next "
            "restart, the node will resume syncing from %d "
            "without using any snapshot data. "
            "Please report this incident to %s, including how you obtained the snapshot. "
            "The invalid snapshot chainstate will be left on disk in case it is "
            "helpful in diagnosing the issue that caused this error."),
            CLIENT_NAME, unvalidated_cs.m_chain.Height(),
            validated_cs.m_chain.Height(),
            validated_cs.m_chain.Height(), CLIENT_BUGREPORT);

        LogError("[snapshot] !!! %s\n", user_error.original);
        LogError("[snapshot] deleting snapshot, reverting to validated chain, and stopping node\n");

        // Reset chainstate target to network tip instead of snapshot block.
        validated_cs.SetTargetBlock(nullptr);

        unvalidated_cs.m_assumeutxo = Assumeutxo::INVALID;

        auto rename_result = unvalidated_cs.InvalidateCoinsDBOnDisk();
        if (!rename_result) {
            user_error += Untranslated("\n") + util::ErrorString(rename_result);
        }

        GetNotifications().fatalError(user_error);
    };

    CCoinsViewDB& validated_coins_db = validated_cs.CoinsDB();
    validated_cs.ForceFlushStateToDisk();

    const auto& maybe_au_data = m_options.chainparams.AssumeutxoForHeight(validated_cs.m_chain.Height());
    if (!maybe_au_data) {
        LogWarning("[snapshot] assumeutxo data not found for height "
            "(%d) - refusing to validate snapshot", validated_cs.m_chain.Height());
        handle_invalid_snapshot();
        return SnapshotCompletionResult::MISSING_CHAINPARAMS;
    }

    const AssumeutxoData& au_data = *maybe_au_data;
    std::optional<CCoinsStats> validated_cs_stats;
    LogInfo("[snapshot] computing UTXO stats for background chainstate to validate "
        "snapshot - this could take a few minutes");
    try {
        validated_cs_stats = ComputeUTXOStats(
            CoinStatsHashType::HASH_SERIALIZED,
            validated_coins_db,
            m_blockman,
            [&interrupt = m_interrupt] { SnapshotUTXOHashBreakpoint(interrupt); });
    } catch (StopHashingException const&) {
        return SnapshotCompletionResult::STATS_FAILED;
    }

    // XXX note that this function is slow and will hold cs_main for potentially minutes.
    if (!validated_cs_stats) {
        LogWarning("[snapshot] failed to generate stats for validation coins db");
        // While this isn't a problem with the snapshot per se, this condition
        // prevents us from validating the snapshot, so we should shut down and let the
        // user handle the issue manually.
        handle_invalid_snapshot();
        return SnapshotCompletionResult::STATS_FAILED;
    }

    // Compare the validated chainstate's UTXO set hash against the hard-coded
    // assumeutxo hash we expect.
    //
    // TODO: For belt-and-suspenders, we could cache the UTXO set
    // hash for the snapshot when it's loaded in its chainstate's leveldb. We could then
    // reference that here for an additional check.
    if (AssumeutxoHash{validated_cs_stats->hashSerialized} != au_data.hash_serialized) {
        LogWarning("[snapshot] hash mismatch: actual=%s, expected=%s",
            validated_cs_stats->hashSerialized.ToString(),
            au_data.hash_serialized.ToString());
        handle_invalid_snapshot();
        return SnapshotCompletionResult::HASH_MISMATCH;
    }

    LogInfo("[snapshot] snapshot beginning at %s has been fully validated",
        unvalidated_cs.m_from_snapshot_blockhash->ToString());

    unvalidated_cs.m_assumeutxo = Assumeutxo::VALIDATED;
    validated_cs.m_target_utxohash = AssumeutxoHash{validated_cs_stats->hashSerialized};
    this->MaybeRebalanceCaches();

    return SnapshotCompletionResult::SUCCESS;
}

Chainstate& ChainstateManager::ActiveChainstate() const
{
    LOCK(::cs_main);
    return CurrentChainstate();
}

void ChainstateManager::MaybeRebalanceCaches()
{
    AssertLockHeld(::cs_main);
    Chainstate& current_cs{CurrentChainstate()};
    Chainstate* historical_cs{HistoricalChainstate()};
    if (!historical_cs && !current_cs.m_from_snapshot_blockhash) {
        // Allocate everything to the IBD chainstate. This will always happen
        // when we are not using a snapshot.
        current_cs.ResizeCoinsCaches(m_total_coinstip_cache, m_total_coinsdb_cache);
    } else if (!historical_cs) {
        // If background validation has completed and snapshot is our active chain...
        LogInfo("[snapshot] allocating all cache to the snapshot chainstate");
        // Allocate everything to the snapshot chainstate.
        current_cs.ResizeCoinsCaches(m_total_coinstip_cache, m_total_coinsdb_cache);
    } else {
        // If both chainstates exist, determine who needs more cache based on IBD status.
        //
        // Note: shrink caches first so that we don't inadvertently overwhelm available memory.
        if (IsInitialBlockDownload()) {
            historical_cs->ResizeCoinsCaches(
                m_total_coinstip_cache * 0.05, m_total_coinsdb_cache * 0.05);
            current_cs.ResizeCoinsCaches(
                m_total_coinstip_cache * 0.95, m_total_coinsdb_cache * 0.95);
        } else {
            current_cs.ResizeCoinsCaches(
                m_total_coinstip_cache * 0.05, m_total_coinsdb_cache * 0.05);
            historical_cs->ResizeCoinsCaches(
                m_total_coinstip_cache * 0.95, m_total_coinsdb_cache * 0.95);
        }
    }
}

void ChainstateManager::ResetChainstates()
{
    m_chainstates.clear();
}

/**
 * Apply default chain params to nullopt members.
 * This helps to avoid coding errors around the accidental use of the compare
 * operators that accept nullopt, thus ignoring the intended default value.
 */
static ChainstateManager::Options&& Flatten(ChainstateManager::Options&& opts)
{
    if (!opts.check_block_index.has_value()) opts.check_block_index = opts.chainparams.DefaultConsistencyChecks();
    if (!opts.minimum_chain_work.has_value()) opts.minimum_chain_work = UintToArith256(opts.chainparams.GetConsensus().nMinimumChainWork);
    if (!opts.assumed_valid_block.has_value()) opts.assumed_valid_block = opts.chainparams.GetConsensus().defaultAssumeValid;
    return std::move(opts);
}

ChainstateManager::ChainstateManager(const util::SignalInterrupt& interrupt, Options options, node::BlockManager::Options blockman_options)
    : m_script_check_queue{/*batch_size=*/128, std::clamp(options.worker_threads_num, 0, MAX_SCRIPTCHECK_THREADS)},
      m_interrupt{interrupt},
      m_options{Flatten(std::move(options))},
      m_blockman{interrupt, std::move(blockman_options)},
      m_validation_cache{m_options.script_execution_cache_bytes, m_options.signature_cache_bytes}
{
    if (GetConsensus().sidechain.enabled) {
        // The record of the mainchain lives next to the block index, but is not wiped with it:
        // it is what the blocks on disk are checked against when the index is rebuilt.
        DBParams db_params{m_blockman.m_opts.block_tree_db_params};
        db_params.path = db_params.path.parent_path() / "mainchain";
        db_params.cache_bytes = 2_MiB;
        db_params.wipe_data = false;
        m_mainchain = std::make_unique<sidechain::Mainchain>(db_params);
    }
}

ChainstateManager::~ChainstateManager()
{
    LOCK(::cs_main);

    m_versionbitscache.Clear();
}

Chainstate* ChainstateManager::LoadAssumeutxoChainstate()
{
    assert(!CurrentChainstate().m_from_snapshot_blockhash);
    std::optional<fs::path> path = node::FindAssumeutxoChainstateDir(m_options.datadir);
    if (!path) {
        return nullptr;
    }
    std::optional<uint256> base_blockhash = node::ReadSnapshotBaseBlockhash(*path);
    if (!base_blockhash) {
        return nullptr;
    }
    LogInfo("[snapshot] detected active snapshot chainstate (%s) - loading",
        fs::PathToString(*path));

    auto snapshot_chainstate{std::make_unique<Chainstate>(nullptr, m_blockman, *this, base_blockhash)};
    LogInfo("[snapshot] switching active chainstate to %s", snapshot_chainstate->ToString());
    return &this->AddChainstate(std::move(snapshot_chainstate));
}

Chainstate& ChainstateManager::AddChainstate(std::unique_ptr<Chainstate> chainstate)
{
    Chainstate& prev_chainstate{CurrentChainstate()};
    assert(prev_chainstate.m_assumeutxo == Assumeutxo::VALIDATED);
    // Set target block for historical chainstate to snapshot block.
    assert(!prev_chainstate.TargetBlockHash());
    prev_chainstate.SetTargetBlockHash(*Assert(chainstate->m_from_snapshot_blockhash));
    m_chainstates.push_back(std::move(chainstate));
    Chainstate& curr_chainstate{CurrentChainstate()};
    assert(&curr_chainstate == m_chainstates.back().get());

    // Transfer possession of the mempool to the chainstate.
    // Mempool is empty at this point because we're still in IBD.
    assert(!prev_chainstate.m_mempool || prev_chainstate.m_mempool->size() == 0);
    assert(!curr_chainstate.m_mempool);
    std::swap(curr_chainstate.m_mempool, prev_chainstate.m_mempool);
    return curr_chainstate;
}

bool IsBIP30Repeat(const CBlockIndex& block_index)
{
    return (block_index.nHeight==91842 && block_index.GetBlockHash() == uint256{"00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec"}) ||
           (block_index.nHeight==91880 && block_index.GetBlockHash() == uint256{"00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721"});
}

bool IsBIP30Unspendable(const uint256& block_hash, int block_height)
{
    return (block_height==91722 && block_hash == uint256{"00000000000271a2dc26e7667f8419f2e15416dc6955e5a6c6cdf3f2574dd08e"}) ||
           (block_height==91812 && block_hash == uint256{"00000000000af0aed4792b1acee3d966af36cf5def14935db8de83d6f9306f2f"});
}

util::Result<void> Chainstate::InvalidateCoinsDBOnDisk()
{
    // Should never be called on a non-snapshot chainstate.
    assert(m_from_snapshot_blockhash);

    // Coins views no longer usable.
    m_coins_views.reset();

    const fs::path db_path{StoragePath()};
    const fs::path invalid_path{db_path + "_INVALID"};
    const std::string db_path_str{fs::PathToString(db_path)};
    const std::string invalid_path_str{fs::PathToString(invalid_path)};
    LogInfo("[snapshot] renaming snapshot datadir %s to %s", db_path_str, invalid_path_str);

    // The invalid storage directory is simply moved and not deleted because we may
    // want to do forensics later during issue investigation. The user is instructed
    // accordingly in MaybeValidateSnapshot().
    try {
        fs::rename(db_path, invalid_path);
    } catch (const fs::filesystem_error& e) {
        LogError("While invalidating the coins db: Error renaming file '%s' -> '%s': %s",
                 db_path_str, invalid_path_str, e.what());
        return util::Error{strprintf(_(
            "Rename of '%s' -> '%s' failed. "
            "You should resolve this by manually moving or deleting the invalid "
            "snapshot directory %s, otherwise you will encounter the same error again "
            "on the next startup."),
            db_path_str, invalid_path_str, db_path_str)};
    }
    return {};
}

bool ChainstateManager::DeleteChainstate(Chainstate& chainstate)
{
    AssertLockHeld(::cs_main);
    assert(!chainstate.m_coins_views);
    const fs::path db_path{chainstate.StoragePath()};
    if (!DeleteCoinsDBFromDisk(db_path, /*is_snapshot=*/bool{chainstate.m_from_snapshot_blockhash})) {
        LogError("Deletion of %s failed. Please remove it manually to continue reindexing.",
                  fs::PathToString(db_path));
        return false;
    }
    // Its sidechain database and the store of its sidechain state go with it: nothing else would
    // ever wipe them (a reindex of the chainstate rebuilds only the chainstate it keeps).
    if (chainstate.m_from_snapshot_blockhash && m_blockman.m_drivechain_db) {
        const std::string name{chainstate.DrivechainStateName()};
        m_blockman.m_drivechain_db->WipeState(name, *m_blockman.m_drivechain_db->SideStore(name));
    }
    std::unique_ptr<Chainstate> prev_chainstate{Assert(RemoveChainstate(chainstate))};
    Chainstate& curr_chainstate{CurrentChainstate()};
    assert(!prev_chainstate->m_mempool || prev_chainstate->m_mempool->size() == 0);
    assert(!curr_chainstate.m_mempool);
    std::swap(curr_chainstate.m_mempool, prev_chainstate->m_mempool);
    return true;
}

ChainstateRole Chainstate::GetRole() const
{
    return ChainstateRole{.validated = m_assumeutxo == Assumeutxo::VALIDATED, .historical = bool{m_target_blockhash}};
}

void ChainstateManager::RecalculateBestHeader()
{
    AssertLockHeld(cs_main);
    m_best_header = ActiveChain().Tip();
    for (auto& entry : m_blockman.m_block_index) {
        if (!(entry.second.nStatus & BLOCK_FAILED_VALID) && m_best_header->nChainWork < entry.second.nChainWork) {
            m_best_header = &entry.second;
        }
    }
}

std::optional<int> ChainstateManager::BlocksAheadOfTip() const
{
    LOCK(::cs_main);
    const CBlockIndex* best_header{m_best_header};
    const CBlockIndex* tip{ActiveChain().Tip()};
    // Only consider headers that extend the active tip; ignore competing branches.
    if (best_header && tip && best_header->nChainWork > tip->nChainWork &&
        best_header->GetAncestor(tip->nHeight) == tip) {
        return best_header->nHeight - tip->nHeight;
    }
    return std::nullopt;
}

bool ChainstateManager::ValidatedSnapshotCleanup(Chainstate& validated_cs, Chainstate& unvalidated_cs)
{
    AssertLockHeld(::cs_main);
    if (unvalidated_cs.m_assumeutxo != Assumeutxo::VALIDATED) {
        // No need to clean up.
        return false;
    }

    const fs::path validated_path{validated_cs.StoragePath()};
    const fs::path assumed_valid_path{unvalidated_cs.StoragePath()};
    const fs::path delete_path{validated_path + "_todelete"};

    // The snapshot chainstate becomes the chainstate: from then on its sidechain state is the one the
    // background chainstate wrote, brought forward with the blocks. What it kept under its own name goes.
    if (unvalidated_cs.m_from_snapshot_blockhash) {
        const std::string name{unvalidated_cs.DrivechainStateName()};
        m_blockman.m_drivechain_db->WipeState(name, *m_blockman.m_drivechain_db->SideStore(name));
    }

    // Since we're going to be moving around the underlying leveldb filesystem content
    // for each chainstate, make sure that the chainstates (and their constituent
    // CoinsViews members) have been destructed first.
    //
    // The caller of this method will be responsible for reinitializing chainstates
    // if they want to continue operation.
    this->ResetChainstates();
    assert(this->m_chainstates.size() == 0);

    LogInfo("[snapshot] deleting background chainstate directory (now unnecessary) (%s)",
              fs::PathToString(validated_path));

    auto rename_failed_abort = [this](
                                   fs::path p_old,
                                   fs::path p_new,
                                   const fs::filesystem_error& err) {
        LogError("[snapshot] Error renaming path (%s) -> (%s): %s\n",
                  fs::PathToString(p_old), fs::PathToString(p_new), err.what());
        GetNotifications().fatalError(strprintf(_(
            "Rename of '%s' -> '%s' failed. "
            "Cannot clean up the background chainstate leveldb directory."),
            fs::PathToString(p_old), fs::PathToString(p_new)));
    };

    try {
        fs::rename(validated_path, delete_path);
    } catch (const fs::filesystem_error& e) {
        rename_failed_abort(validated_path, delete_path, e);
        throw;
    }

    LogInfo("[snapshot] moving snapshot chainstate (%s) to "
              "default chainstate directory (%s)",
              fs::PathToString(assumed_valid_path), fs::PathToString(validated_path));

    try {
        fs::rename(assumed_valid_path, validated_path);
    } catch (const fs::filesystem_error& e) {
        rename_failed_abort(assumed_valid_path, validated_path, e);
        throw;
    }

    if (!DeleteCoinsDBFromDisk(delete_path, /*is_snapshot=*/false)) {
        // No need to FatalError because once the unneeded bg chainstate data is
        // moved, it will not interfere with subsequent initialization.
        LogWarning("Deletion of %s failed. Please remove it manually, as the "
                   "directory is now unnecessary.",
                   fs::PathToString(delete_path));
    } else {
        LogInfo("[snapshot] deleted background chainstate directory (%s)",
                fs::PathToString(validated_path));
    }
    return true;
}

std::pair<int, int> Chainstate::GetPruneRange(int last_height_can_prune) const
{
    if (m_chain.Height() <= 0) {
        return {0, 0};
    }
    int prune_start{0};

    if (m_from_snapshot_blockhash && m_assumeutxo != Assumeutxo::VALIDATED) {
        // Only prune blocks _after_ the snapshot if this is a snapshot chain
        // that has not been fully validated yet. The earlier blocks need to be
        // kept to validate the snapshot
        prune_start = Assert(SnapshotBase())->nHeight + 1;
    }

    // (On a sidechain, the blocks any reorg can take back: MinBlocksToKeep.)
    int max_prune = std::max<int>(
        0, m_chain.Height() - MinBlocksToKeep(m_chainman.GetConsensus()));

    // last block to prune is the lesser of (caller-specified height, MIN_BLOCKS_TO_KEEP from the tip)
    //
    // While you might be tempted to prune the background chainstate more
    // aggressively (i.e. fewer MIN_BLOCKS_TO_KEEP), this won't work with index
    // building - specifically blockfilterindex requires undo data, and if
    // we don't maintain this trailing window, we hit indexing failures.
    int prune_end = std::min(last_height_can_prune, max_prune);

    return {prune_start, prune_end};
}

std::optional<std::pair<const CBlockIndex*, const CBlockIndex*>> ChainstateManager::GetHistoricalBlockRange() const
{
    const Chainstate* chainstate{HistoricalChainstate()};
    if (!chainstate) return {};
    return std::make_pair(chainstate->m_chain.Tip(), chainstate->TargetBlock());
}

util::Result<void> ChainstateManager::ActivateBestChains()
{
    // We can't hold cs_main during ActivateBestChain even though we're accessing
    // the chainman unique_ptrs since ABC requires us not to be holding cs_main, so retrieve
    // the relevant pointers before the ABC call.
    AssertLockNotHeld(cs_main);
    std::vector<Chainstate*> chainstates;
    {
        LOCK(GetMutex());
        chainstates.reserve(m_chainstates.size());
        for (const auto& chainstate : m_chainstates) {
            if (chainstate && chainstate->m_assumeutxo != Assumeutxo::INVALID && !chainstate->m_target_utxohash) {
                chainstates.push_back(chainstate.get());
            }
        }
    }
    for (Chainstate* chainstate : chainstates) {
        BlockValidationState state;
        if (!chainstate->ActivateBestChain(state, nullptr)) {
            LOCK(GetMutex());
            return util::Error{Untranslated(strprintf("%s Failed to connect best block (%s)", chainstate->ToString(), state.ToString()))};
        }
    }
    return {};
}

void ChainstateManager::AddBmmWaiting(const CBlockHeader& header, int64_t peer) const
{
    AssertLockHeld(::cs_main);
    static constexpr size_t MAX_BMM_WAITING{1000};
    const uint256 hash{header.GetHash()};
    if (const auto it{m_bmm_waiting.find(hash)}; it != m_bmm_waiting.end()) {
        // A header without a peer was looked at by validation; the peer that sent it is the one to keep.
        if (it->second.peer < 0) it->second.peer = peer;
        return;
    }
    if (m_bmm_waiting.size() >= MAX_BMM_WAITING) {
        std::map<int64_t, size_t> counts;
        for (const auto& [_, waiting] : m_bmm_waiting) ++counts[waiting.peer];
        const int64_t crowded{std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first};
        auto oldest{m_bmm_waiting.end()};
        for (auto it{m_bmm_waiting.begin()}; it != m_bmm_waiting.end(); ++it) {
            if (it->second.peer == crowded && (oldest == m_bmm_waiting.end() || it->second.sequence < oldest->second.sequence)) oldest = it;
        }
        m_bmm_waiting.erase(oldest);
    }
    m_bmm_waiting.emplace(hash, BmmWaiting{header, peer, m_bmm_waiting_sequence++});
}

std::vector<std::pair<uint256, int64_t>> ChainstateManager::RetryBmmWaiting()
{
    AssertLockNotHeld(::cs_main);
    std::vector<std::pair<CBlockHeader, int64_t>> pending;
    {
        LOCK(::cs_main);
        if (!m_mainchain) return {};
        std::multimap<int64_t, std::pair<CBlockHeader, int64_t>> ready;
        for (const auto& [_, waiting] : m_bmm_waiting) {
            if (const auto order{sidechain::Hooks().WaitingReady(waiting.header, *m_mainchain)}) ready.emplace(*order, std::make_pair(waiting.header, waiting.peer));
        }
        for (auto& [_, entry] : ready) pending.push_back(std::move(entry));
    }
    // Each stays in the store while it is tried: validation, finding it still waits, keeps it there with
    // its peer, where a header taken out would come back without one (and the block never be fetched).
    std::vector<std::pair<uint256, int64_t>> taken;
    while (!pending.empty()) {
        // A header the hooks put before its parent (the same key, say) is tried again after it, as long
        // as each pass takes another header in.
        std::vector<std::pair<CBlockHeader, int64_t>> orphans;
        bool progress{false};
        for (const auto& [header, peer] : pending) {
            const uint256 hash{header.GetHash()};
            BlockValidationState state;
            const CBlockIndex* pindex{nullptr};
            // A header whose sender is not known is not the node's own: it holds no unchecked header.
            const bool accepted{ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state, &pindex, peer < 0 ? UNKNOWN_SOURCE : peer) && pindex};
            LOCK(::cs_main);
            const auto it{m_bmm_waiting.find(hash)};
            if (accepted) {
                // The peer may have been learned since the header was looked up.
                const int64_t from{it != m_bmm_waiting.end() && it->second.peer >= 0 ? it->second.peer : peer};
                if (it != m_bmm_waiting.end()) m_bmm_waiting.erase(it);
                taken.emplace_back(hash, from);
                progress = true;
            } else if (state.GetResult() == BlockValidationResult::BLOCK_MISSING_PREV) {
                orphans.emplace_back(header, peer);
            } else if (state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE && sidechain::Hooks().IsWaitReason(state.GetRejectReason())) {
                // Still waiting: kept, with its peer (put back if it was made room for meanwhile).
                AddBmmWaiting(header, peer);
            } else if (it != m_bmm_waiting.end()) {
                m_bmm_waiting.erase(it);
            }
        }
        pending.swap(orphans);
        if (!progress) break;
    }
    // Headers whose parent did not come: they wait as long as it does. Of a parent not waiting, the
    // peer that sent them sends the rest of its chain when asked, as for any header.
    LOCK(::cs_main);
    for (const auto& [header, _] : pending) {
        if (!m_bmm_waiting.contains(header.hashPrevBlock)) m_bmm_waiting.erase(header.GetHash());
    }
    return taken;
}
