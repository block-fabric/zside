// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_FOLLOWER_H
#define BITCOIN_SIDECHAIN_FOLLOWER_H

#include <consensus/amount.h>
#include <primitives/block.h>
#include <script/script.h>
#include <sidechain/mainclient.h>
#include <sync.h>
#include <uint256.h>
#include <util/time.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace node {
struct NodeContext;
} // namespace node

namespace sidechain {
struct MainBlock;

/**
 * Keeps the record of the mainchain up to date, and does what follows from a
 * change of the mainchain:
 *
 *  - when the mainchain drops a block that committed to a block of this chain,
 *    that block is no longer valid, and when the commitment comes back, it is
 *    valid again;
 *  - when the mainchain commits to a block this node built, the block is
 *    connected;
 *  - when this chain has a withdrawal bundle to be voted on, the mainchain
 *    node is given its transaction.
 *
 * It also builds the blocks of this chain and asks the mainchain to commit to
 * them (blind merged mining), on request or continuously.
 */
class Follower
{
public:
    Follower(node::NodeContext& node, MainClient::Options options);
    ~Follower();

    void Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Bring the record of the mainchain up to date, and act on the changes.
     * Must not be called with cs_main held.
     * @return false, with `error` set, if the mainchain node could not be asked.
     */
    bool Sync(std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    /**
     * Bring the record up to date if no other thread is doing so, and leave
     * acting on the changes to the thread of the follower. For callers that
     * hold cs_main.
     */
    void Poll() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);

    struct Status {
        bool connected{false};
        std::string error;
    };
    Status GetStatus() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** A block of this chain waiting for the mainchain to commit to it. */
    struct Candidate {
        std::shared_ptr<const CBlock> block;
        //! The mainchain block the candidate was built on; only the next one can commit to it.
        uint256 main_prev_hash;
        int main_prev_height{0};
    };
    /**
     * Build a block that pays its fees to `coinbase_script`, and keep it until
     * the mainchain commits to it or moves on.
     */
    std::optional<Candidate> CreateCandidate(const CScript& coinbase_script, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    /** Ask the wallet of the mainchain node to pay `amount` for a commitment to `candidate`. @return the id of its transaction */
    std::optional<std::string> RequestCommitment(const Candidate& candidate, CAmount amount, std::string& error) const;

    struct Mining {
        bool enabled{false};
        CScript coinbase_script;
        //! Mine a block for every block of the mainchain, with or without fees, and offer `amount` for each. For tests.
        bool always{false};
        CAmount amount{0};
        //! Whether mining is on but waits, because a block would not have the fees to pay for itself.
        bool idle{false};
        //! The fees of the block last asked for, and what was offered for it.
        CAmount last_fees{0};
        CAmount last_offer{0};
        uint64_t requests{0};
        uint64_t blocks{0};
        //! Mainchain blocks for which another node's bid, for another block of this chain, paid more.
        uint64_t outbid{0};
        std::string error;
    };
    /**
     * What is offered to the miners of the mainchain for a block with these
     * fees, when mining continuously: all but the share of the operator of
     * this node, one hundredth.
     */
    static CAmount Offer(CAmount fees) { return fees - fees / 100; }
    //! The least that is offered: below it, the request would not pay for its own place in a block of the mainchain.
    static constexpr CAmount MIN_OFFER{500};
    /** Turn continuous merged mining on or off. */
    /**
     * Turn continuous merged mining on or off. It asks for a block when the
     * block has fees, and offers the miners of the mainchain those fees less
     * the share of the operator; a block without fees is not asked for.
     */
    void SetMining(bool enabled, const CScript& coinbase_script, bool always = false, CAmount amount = 0) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Give up a candidate that no commitment will be asked for. */
    void DropCandidate(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Mining GetMining() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const MainClient& Client() const { return m_client; }

private:
    void Run() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    /** Update the record. @return whether it changed */
    /**
     * @param[in] may_drop  whether to follow the mainchain back when it dropped blocks on record;
     *                      Poll, called with the main lock held, leaves that to the thread of the follower
     */
    bool UpdateRecord(bool may_drop = true) EXCLUSIVE_LOCKS_REQUIRED(m_sync_mutex, !m_mutex);
    /** Do what the changes of the record call for. */
    void Act() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Mine() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    void SendBundle() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Once, when the record has caught up: drop from the active chain the blocks the mainchain does not commit to. */
    void CheckActiveChain() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! How far below the tip of the mainchain record commitments are checked at start (CheckActiveChain).
    static constexpr int MOVED_CHECK_DEPTH{100};
    //! Least time between two calls to the mainchain node on behalf of block headers (Poll).
    static constexpr auto POLL_SPACING{std::chrono::seconds{2}};

    node::NodeContext& m_node;
    const MainClient m_client;

    //! Held while the record is updated.
    Mutex m_sync_mutex;

    mutable Mutex m_mutex;
    std::condition_variable_any m_wake;
    std::thread m_thread GUARDED_BY(m_mutex);
    std::atomic<bool> m_stop{false};
    //! Set when the thread has something to do before its next regular round.
    bool m_woken GUARDED_BY(m_mutex){false};
    Status m_status GUARDED_BY(m_mutex);
    //! Sidechain blocks that lost, respectively gained, their commitment, and are still to be acted on.
    std::vector<uint256> m_uncommitted GUARDED_BY(m_mutex);
    std::vector<uint256> m_committed GUARDED_BY(m_mutex);
    std::map<uint256, Candidate> m_candidates GUARDED_BY(m_mutex);
    Mining m_mining GUARDED_BY(m_mutex);
    //! Counts the calls of SetMining, for the thread to tell that the settings changed under it.
    uint64_t m_mining_settings GUARDED_BY(m_mutex){0};
    //! The mainchain block for which a commitment was last asked.
    uint256 m_mining_requested_for GUARDED_BY(m_mutex);
    //! The bundle the mainchain node was last given.
    uint256 m_bundle_sent GUARDED_BY(m_mutex);
    //! What the mainchain node was last told this chain vouches for: a bundle, or none (null); not told yet if empty.
    std::optional<uint256> m_vouched GUARDED_BY(m_mutex);
    bool m_vouch_warned GUARDED_BY(m_mutex){false};
    //! The bundle the mainchain node last refused, so that it is said once.
    uint256 m_bundle_refused GUARDED_BY(m_mutex);
    //! Held while acting on changes of the record, which Sync callers on several threads do.
    Mutex m_act_mutex;
    NodeClock::time_point m_last_poll GUARDED_BY(m_mutex){};
    bool m_chain_checked GUARDED_BY(m_mutex){false};
    //! The bundle whose payout was last asked of the mainchain node, and at which mainchain height.
    std::pair<uint256, int> m_payout_tried GUARDED_BY(m_mutex){uint256{}, -1};
};

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_FOLLOWER_H
