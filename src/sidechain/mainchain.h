// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_MAINCHAIN_H
#define BITCOIN_SIDECHAIN_MAINCHAIN_H

#include <consensus/amount.h>
#include <dbwrapper.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sidechain {

/** Coins that a mainchain transaction added to the escrow of this sidechain. */
struct MainDeposit {
    //! Where the depositor wants the coins on this chain; free text chosen by the depositor.
    std::string destination;
    CAmount amount{0};
    uint256 txid;
    uint32_t burn_index{0};
    //! For the change of a withdrawal: the bundle it paid out, and what it paid.
    uint256 bundle;
    std::vector<CTxOut> payouts;

    SERIALIZE_METHODS(MainDeposit, obj) { READWRITE(obj.destination, obj.amount, obj.txid, obj.burn_index, obj.bundle, obj.payouts); }
    friend bool operator==(const MainDeposit&, const MainDeposit&) = default;
};

/** A withdrawal bundle of this sidechain that a mainchain block closed. */
struct MainBundleEvent {
    uint256 hash;
    //! Whether the bundle was paid out; if not, it failed.
    bool paid{false};

    SERIALIZE_METHODS(MainBundleEvent, obj) { READWRITE(obj.hash, obj.paid); }
    friend bool operator==(const MainBundleEvent&, const MainBundleEvent&) = default;
};

/** A withdrawal bundle of this sidechain pending on the mainchain after a block, with its work score then. */
struct MainPendingBundle {
    uint256 hash;
    uint32_t score{0};

    SERIALIZE_METHODS(MainPendingBundle, obj) { READWRITE(obj.hash, obj.score); }
    friend bool operator==(const MainPendingBundle&, const MainPendingBundle&) = default;
};

/** What a mainchain block did that concerns this sidechain. */
struct MainBlock {
    uint256 hash;
    uint256 prev_hash;
    //! The median time of the block (see getsidechainevents), which miners cannot move ahead.
    int64_t time{0};
    //! Hash of the block of this sidechain that the mainchain block committed to (blind merged mining).
    std::optional<uint256> bmm;
    std::vector<MainDeposit> deposits;
    std::vector<MainBundleEvent> bundles;
    //! The withdrawal bundles of this sidechain that the block proposed (M3): pending until closed.
    std::vector<uint256> proposed;
    //! The withdrawal bundles of this sidechain pending after the block, with their scores.
    std::vector<MainPendingBundle> pending;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << hash << prev_hash << time << bmm.has_value();
        if (bmm) s << *bmm;
        s << deposits << bundles << proposed << pending;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bool has_bmm;
        s >> hash >> prev_hash >> time >> has_bmm;
        if (has_bmm) s >> bmm.emplace();
        s >> deposits >> bundles;
        // Records written before proposals, then the pending bundles, were kept end here: Mainchain
        // fills them in (NeedsBackfill).
        proposed.clear();
        pending.clear();
        if constexpr (requires { s.empty(); }) {
            if (!s.empty()) s >> proposed;
            if (!s.empty()) s >> pending;
        } else {
            s >> proposed >> pending;
        }
    }
    friend bool operator==(const MainBlock&, const MainBlock&) = default;
};

/** Which sidechain holds the slot of this chain on the mainchain: what the Follower saw first. */
struct SlotIdentity {
    //! Height of the mainchain block that activated the sidechain in the slot.
    int32_t activation_height{0};
    //! Hash of the proposal that activated it, which describes the sidechain.
    uint256 proposal_hash;

    SERIALIZE_METHODS(SlotIdentity, obj) { READWRITE(obj.activation_height, obj.proposal_hash); }
    friend bool operator==(const SlotIdentity&, const SlotIdentity&) = default;
};

/**
 * The record this node keeps of the mainchain: for every block of its active
 * chain, what the block did that concerns this sidechain.
 *
 * All sidechain rules that depend on the mainchain are checked against this
 * record, never against the mainchain node directly. That makes the validity
 * of a sidechain block a function of the block, the blocks before it and the
 * record. The record itself is kept up to date by the Follower.
 */
class Mainchain
{
public:
    /** @param[in] db_params  where to keep the record; in memory only if not given */
    explicit Mainchain(const std::optional<DBParams>& db_params = std::nullopt);

    /** Height of the last block on record, -1 if there is none. */
    int Height() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Hash of the last block on record; null if there is none. */
    uint256 TipHash() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<MainBlock> GetBlock(int height) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Height of the mainchain block that committed to the sidechain block
     * `side_hash`, if any did.
     */
    std::optional<int> BmmHeight(const uint256& side_hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Like BmmHeight, but only what is on record: never a commitment assumed by AssumeCommitted. */
    std::optional<int> CommittedHeight(const uint256& side_hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Height of the first mainchain block that closed (paid out or failed) the bundle `hash`, if any did. */
    std::optional<int> ClosedHeight(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether a mainchain block from `from` to `to` proposed the bundle `hash`. */
    bool ProposedBetween(const uint256& hash, int from, int to) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Whether a withdrawal bundle of this sidechain was pending on the mainchain after the block at
     * `main_height`, as the proposals and closes on record say: proposed at or below it, and not
     * closed since. Each proposal and each close counts: a bundle closed, forgotten by the mainchain
     * and proposed again is pending again. Within a block, closes come before proposals. Such a
     * bundle may hold any withdrawal, those of other branches of this chain too.
     */
    bool BundlePending(int main_height) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Whether a bundle other than `ours` was pending on the mainchain after the block at
     * `main_height` (the last one on record if above it) with a work score of `min_score` or more,
     * as the mainchain said (MainBlock::pending).
     */
    bool SupportedPending(int main_height, uint32_t min_score, const uint256& ours) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The sidechain block that the mainchain block at `height` committed to, if any. */
    std::optional<uint256> BmmAt(int height) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Whether the record was written before it kept the proposals of bundles: then the Follower
     * fills them in for the blocks on record (Backfill), before anything acts on them.
     */
    bool NeedsBackfill() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Set the proposals and pending bundles of the block on record at `height`, if it is the block `hash`.
     * @return false if the record has another block there.
     */
    bool Backfill(int height, const uint256& hash, const std::vector<uint256>& proposed, const std::vector<MainPendingBundle>& pending = {}) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The record is complete again: mark it so, with the blocks checked without it to be checked again (RecheckPending). */
    void BackfillDone() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Whether the blocks of the active chain, connected while the record missed the proposals, are still
     * to be checked again against the complete record; the Follower does so, then clears it.
     */
    bool RecheckPending() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void RecheckDone() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Why a block of this chain is marked failed, as far as the Follower is concerned: whether it may
     * take the failure back when the record changes.
     */
    enum class Failure : uint8_t {
        //! The block failed against the record (its commitment lost or moved, a rule of the sidechain
        //! as the record stood): it is checked again when its commitment comes back.
        RECORD = 1,
        //! The operator marked it invalid (invalidateblock): it stays so, and what is built on it.
        MANUAL = 2,
    };
    void NoteFailure(const uint256& block_hash, Failure failure) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<Failure> GetFailure(const uint256& block_hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ForgetFailure(const uint256& block_hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The sidechain this node found in its slot, once it found one. */
    std::optional<SlotIdentity> GetSlotIdentity() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetSlotIdentity(const SlotIdentity& identity) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Add the block that follows the last one on record. @return false if it does not follow it. */
    bool Append(const MainBlock& block) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Forget the blocks above `height`. @return the forgotten blocks, lowest first. */
    std::vector<MainBlock> Truncate(int height) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * While an object of this class lives, sidechain blocks that no mainchain
     * block committed to are taken to be committed at `height`. This is how a
     * block is checked before it is offered to the mainchain for merged mining.
     */
    class AssumeCommitted
    {
    public:
        AssumeCommitted(Mainchain& mainchain, int height) : m_mainchain{mainchain}
        {
            LOCK(m_mainchain.m_mutex);
            m_mainchain.m_assumed_height = height;
        }
        ~AssumeCommitted()
        {
            LOCK(m_mainchain.m_mutex);
            m_mainchain.m_assumed_height.reset();
        }

    private:
        Mainchain& m_mainchain;
    };

private:
    mutable Mutex m_mutex;
    std::vector<MainBlock> m_blocks GUARDED_BY(m_mutex);
    //! Sidechain block hash to the height of the mainchain block that committed to it.
    std::map<uint256, int> m_bmm GUARDED_BY(m_mutex);
    //! Bundle hash to the heights of the mainchain blocks that closed it.
    std::map<uint256, std::set<int>> m_closed GUARDED_BY(m_mutex);
    //! Bundle hash to the heights of the mainchain blocks that proposed it.
    std::map<uint256, std::set<int>> m_proposed GUARDED_BY(m_mutex);
    /** Note the bundles a block on record at `height` proposed and closed (`add`), or forget them. */
    void IndexEvents(const MainBlock& block, int height, bool add) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    /** Whether the bundle `hash` was pending after the block at `height`, by its proposals and closes. */
    bool PendingAfter(const uint256& hash, int height) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    /**
     * Number of bundles pending after each block on record (BundlePending), up to its size; the rest
     * is worked out again when asked for.
     */
    mutable std::vector<uint32_t> m_pending GUARDED_BY(m_mutex);
    void UpdatePending() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void InvalidatePending(int height) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { if (static_cast<int>(m_pending.size()) > height) m_pending.resize(std::max(height, 0)); }
    bool m_needs_backfill GUARDED_BY(m_mutex){false};
    bool m_recheck GUARDED_BY(m_mutex){false};
    std::map<uint256, Failure> m_failures GUARDED_BY(m_mutex);
    std::optional<SlotIdentity> m_slot_identity GUARDED_BY(m_mutex);
    std::optional<int> m_assumed_height GUARDED_BY(m_mutex);
    std::unique_ptr<CDBWrapper> m_db;
};

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_MAINCHAIN_H
