// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_MINER_H
#define BITCOIN_DRIVECHAIN_MINER_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace drivechain {

/** What the drivechain rules make a miner add to a block. */
struct BlockAdditions {
    //! Messages for the coinbase, as zero value outputs.
    std::vector<CTxOut> coinbase_outputs;
    //! Withdrawal bundles that can be paid out, to be placed after all other transactions.
    std::vector<CTransactionRef> withdrawals;
    //! Fee of each of the withdrawals.
    std::vector<CAmount> withdrawal_fees;
};

/**
 * What this node does with the drivechain decisions that are left to miners:
 * the sidechains it proposes and acks, the withdrawal bundles it has been
 * handed by sidechains, and how it votes on them. None of this is consensus;
 * it only shapes the blocks this node builds.
 */
class MinerState
{
public:
    /**
     * Keep the state in this file: load what it holds, and write it back
     * whenever something changes.
     * @return false if the file exists but cannot be read
     */
    bool Load(const fs::path& path) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Propose a sidechain in the next block this node builds. */
    void AddProposal(const Sidechain& sidechain) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool RemoveProposal(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<Sidechain> GetProposals() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether to ack the proposal with this hash in the blocks this node builds. */
    /**
     * Acks are for a proposal in a slot: the same sidechain proposed in another slot, or proposed
     * again in the slot it already holds, is another question, which an operator answers anew.
     */
    void SetAck(SidechainId slot, const uint256& proposal_hash, bool ack) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Stop acking a proposal, in every slot. */
    void ClearAcks(const uint256& proposal_hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool IsAcked(SidechainId slot, const uint256& proposal_hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::set<std::pair<SidechainId, uint256>> GetAcks() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Remember a withdrawal bundle received from a sidechain, in blind form.
     * It is proposed in the next block and paid out once it has the work score.
     * @return the hash of the bundle, or nullopt with `error` set
     */
    std::optional<uint256> AddBundle(SidechainId slot, const CMutableTransaction& blind_tx, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<CMutableTransaction> GetBundle(SidechainId slot, const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * The word of the sidechain node of `slot` on its bundle: the one its chain has (handed before with
     * AddBundle), or none. Votes and proposals follow it. @return false if `hash` was never handed.
     */
    bool Vouch(SidechainId slot, const std::optional<uint256>& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** What the sidechain node of `slot` vouches for: nothing known, none (null), or a bundle. */
    std::optional<uint256> Vouched(SidechainId slot) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<std::pair<SidechainId, uint256>> GetBundles() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** How to vote on the bundles of a sidechain. Sidechains without a vote get the default. */
    void SetVote(SidechainId slot, const Vote& vote) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ClearVote(SidechainId slot) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Vote GetVote(SidechainId slot) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * The vote on a sidechain with these pending bundles. Where the default
     * applies and is to upvote, the bundle upvoted is the one a sidechain
     * node handed to this node last (see AddBundle): a bundle it handed before
     * is one its chain gave up, and is not upvoted even if it has more votes.
     * There is none to upvote if the last one handed is not pending.
     */
    Vote ResolveVote(SidechainId slot, const std::vector<Bundle>& pending) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::map<SidechainId, Vote> GetVotes() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetDefaultVote(Vote::Type type) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Vote::Type GetDefaultVote() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Whether, where the default is to upvote and no sidechain node handed this
     * node a bundle, it follows the miners who check them: upvoting the bundle
     * that leads the next by LEADING_BY_50_MARGIN (BIP300 "leading by 50").
     * Off unless the operator opts in, since it backs bundles this node cannot check.
     */
    void SetFollow(bool follow) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool GetFollow() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Drop what the chain has made obsolete: proposals and acks of sidechains that activated, and
     * bundles closed (paid out or failed) PRUNE_DEPTH blocks ago, kept until then in case a reorg
     * makes them pending again.
     */
    void Prune(const SidechainDB& scdb, int height) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    static constexpr int PRUNE_DEPTH{6};

    /**
     * What to add to a block built on top of the block `scdb` belongs to.
     *
     * @param[in] txs  the transactions selected for the block, without the coinbase
     */
    BlockAdditions CreateBlockAdditions(const SidechainDB& scdb, const Consensus::DrivechainParams& params,
                                        const uint256& prev_block_hash, const std::vector<CTransactionRef>& txs) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    /** Write the state to m_path, if there is one. */
    void Save() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    mutable Mutex m_mutex;
    fs::path m_path GUARDED_BY(m_mutex);
    std::vector<Sidechain> m_proposals GUARDED_BY(m_mutex);
    std::set<std::pair<SidechainId, uint256>> m_acks GUARDED_BY(m_mutex);
    std::map<std::pair<SidechainId, uint256>, CMutableTransaction> m_bundles GUARDED_BY(m_mutex);
    //! The bundle a sidechain node handed last, per slot: the one its chain commits to now.
    std::map<SidechainId, uint256> m_latest_bundle GUARDED_BY(m_mutex);
    //! When bundles were first seen closed, for Prune (not saved: a restart only delays pruning).
    std::map<std::pair<SidechainId, uint256>, int> m_closed_seen GUARDED_BY(m_mutex);
    std::map<SidechainId, Vote> m_votes GUARDED_BY(m_mutex);
    //! To upvote is to upvote what the sidechain node of the operator vouches for, and nothing else.
    Vote::Type m_default_vote GUARDED_BY(m_mutex){Vote::Type::UPVOTE};
    bool m_follow GUARDED_BY(m_mutex){false};
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_MINER_H
