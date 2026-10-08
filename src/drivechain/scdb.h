// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_SCDB_H
#define BITCOIN_DRIVECHAIN_SCDB_H

#include <consensus/params.h>
#include <drivechain/sidechain.h>
#include <primitives/block.h>
#include <sidechain/state.h>
#include <serialize.h>
#include <uint256.h>

#include <limits>
#include <map>
#include <optional>
#include <set>
#include <ios>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace drivechain {

/**
 * A hash of the drivechain parameters that decide what the sidechain database becomes for a chain
 * of blocks: all of them. Whatever was derived under other parameters (another activation height,
 * say) is derived anew.
 */
uint256 ParamsFingerprint(const Consensus::DrivechainParams& params);
/**
 * On a chain that is itself a sidechain, with the parameters of its own rules as well: the state of
 * this chain as a sidechain (its store) is derived under them. The same as above on any other chain.
 */
uint256 ParamsFingerprint(const Consensus::DrivechainParams& params, const Consensus::SidechainParams& sidechain);

/** What became of a bundle that is no longer pending. */
struct ClosedBundle {
    //! Whether it was paid out; if not, it failed.
    bool paid{false};
    //! Height of the block that closed it.
    int32_t height{0};
    //! Whether a block ever upvoted it; a failed bundle nobody upvoted is forgotten sooner.
    bool upvoted{true};

    SERIALIZE_METHODS(ClosedBundle, obj) { READWRITE(obj.paid, obj.height, obj.upvoted); }

    friend bool operator==(const ClosedBundle&, const ClosedBundle&) = default;
};

/**
 * What a block changed in the sidechain database, in the form needed to revert it: the changes
 * themselves rather than copies of what they changed, so that it stays small however many bundles
 * are pending.
 */
struct BlockUndo {
    /** What a slot was before the block, apart from its bundles; saved before its first change. */
    struct SlotUndo {
        SidechainId id{0};
        //! Whether the slot held a sidechain before the block.
        bool existed{false};
        int32_t activation_height{0};
        bool has_ctip{false};
        Ctip ctip;
        //! The sidechain the slot held, if the block replaced it with another.
        std::optional<Sidechain> sidechain;

        template <typename Stream>
        void Serialize(Stream& s) const
        {
            s << id << existed << activation_height << has_ctip << ctip << sidechain.has_value();
            if (sidechain) s << *sidechain;
        }
        template <typename Stream>
        void Unserialize(Stream& s)
        {
            bool has_sidechain;
            s >> id >> existed >> activation_height >> has_ctip >> ctip >> has_sidechain;
            sidechain.reset();
            if (has_sidechain) s >> sidechain.emplace();
        }
    };

    /** A change of the pending bundles of a slot. They are taken back in reverse order. */
    struct BundleChange {
        enum class Type : uint8_t {
            //! The bundle at `index` was removed: `bundle`.
            ERASE = 0,
            //! A bundle was added at the end.
            APPEND = 1,
            //! A vote: the bundle at `upvoted` went up (none for a downvote), the others down, except those at `at_zero`.
            VOTE = 2,
        };
        static constexpr uint32_t NO_INDEX{std::numeric_limits<uint32_t>::max()};

        Type type{Type::APPEND};
        SidechainId id{0};
        //! ERASE: the position and the bundle.
        uint32_t index{0};
        Bundle bundle;
        //! VOTE: the position of the bundle upvoted, or NO_INDEX.
        uint32_t upvoted{NO_INDEX};
        //! VOTE: the last upvote of that bundle before, and whether its score could go no higher.
        int32_t last_upvote{0};
        bool saturated{false};
        //! VOTE: the positions, in order, of the bundles a downvote left at a score of zero.
        std::vector<uint32_t> at_zero;

        template <typename Stream>
        void Serialize(Stream& s) const
        {
            s << static_cast<uint8_t>(type) << id;
            switch (type) {
            case Type::ERASE: s << index << bundle; break;
            case Type::APPEND: break;
            case Type::VOTE: s << upvoted << last_upvote << saturated << at_zero; break;
            }
        }
        template <typename Stream>
        void Unserialize(Stream& s)
        {
            uint8_t value;
            s >> value >> id;
            if (value > static_cast<uint8_t>(Type::VOTE)) throw std::ios_base::failure("unknown bundle change");
            type = static_cast<Type>(value);
            switch (type) {
            case Type::ERASE: s >> index >> bundle; break;
            case Type::APPEND: break;
            case Type::VOTE: s >> upvoted >> last_upvote >> saturated >> at_zero; break;
            }
        }
    };

    /** A bundle the block closed. */
    struct Closed {
        SidechainId id{0};
        uint256 hash;
        bool paid{false};

        SERIALIZE_METHODS(Closed, obj) { READWRITE(obj.id, obj.hash, obj.paid); }
    };

    uint256 prev_block_hash;
    //! The slots the block changed, as they were before.
    std::vector<SlotUndo> slots;
    //! The changes of pending bundles, in the order they were made.
    std::vector<BundleChange> bundle_changes;
    //! Number of proposals the block added, at the end of the list.
    uint32_t proposals_added{0};
    //! Slots and hashes of the proposals whose ack count the block incremented.
    std::vector<std::pair<SidechainId, uint256>> acked;
    //! Proposals the block removed, with the position each had when it was removed, in order of removal.
    std::vector<std::pair<uint32_t, Proposal>> removed;
    //! Bundles the block closed, by being paid out or by failing.
    std::vector<Closed> closed;
    //! Failed bundles the block forgot, as they were remembered.
    std::vector<std::pair<std::pair<SidechainId, uint256>, ClosedBundle>> forgotten;
    //! Bundles the block proposed (M3) and that became pending.
    std::vector<std::pair<SidechainId, uint256>> proposed;
    //! The votes of the previous block, which this block replaced.
    std::map<SidechainId, Vote> last_votes;

    //! What the block changed in the state of this chain as a sidechain: the earlier value of
    //! every entry of the store it changed (sidechain/store.h).
    sidechain::StoreUndo side;

    SERIALIZE_METHODS(BlockUndo, obj) { READWRITE(obj.prev_block_hash, obj.slots, obj.bundle_changes, obj.proposals_added, obj.acked, obj.removed, obj.closed, obj.forgotten, obj.proposed, obj.last_votes, obj.side); }
};

/** What the rules of a chain that is itself a sidechain need to check a block. */
struct SideContext {
    const Consensus::SidechainParams& params;
    const sidechain::Mainchain& mainchain;
    //! Set to the coins the coinbase has to create on top of the fees.
    CAmount& minted;
    //! Where the sidechain state is read and written: an overlay that journals, whose journal
    //! becomes BlockUndo::side.
    sidechain::StoreOverlay& store;
    //! Set when the block broke a rule of the sidechain: a verdict that depends on the record of the
    //! mainchain, which may change.
    mutable bool failed{false};
};

/**
 * The sidechain database: the drivechain state implied by a chain of blocks.
 *
 * The state after a block depends on the state before it and on the block
 * alone. In particular the escrow output of every sidechain is tracked here,
 * so that no lookup in the UTXO set is needed. That works because the rules
 * below guarantee that a sidechain never has more than one unspent escrow
 * output.
 */
class SidechainDB
{
public:
    /** The active sidechains with their pending bundles, in slot order; a vote message has one entry per element. */
    using PendingBundles = std::vector<std::pair<SidechainId, std::vector<Bundle>>>;

    /**
     * Check `block`, to be connected at `height` on top of the block this
     * state belongs to, against the drivechain rules, and update the state.
     *
     * @param[out] undo           what is needed to revert the update
     * @param[out] deposits       escrow changes, for the benefit of sidechain software; may be null
     * @param[out] reject_reason  set when the block is invalid
     * @return false if the block is invalid, in which case the state is left
     *         partially updated: DisconnectBlock(undo) takes it back.
     */
    [[nodiscard]] bool ConnectBlock(const CBlock& block, int height, const Consensus::DrivechainParams& params,
                                    BlockUndo& undo, std::vector<Deposit>* deposits, std::string& reject_reason,
                                    const SideContext* side = nullptr);

    /**
     * A copy holding only the sidechains in `slots`: enough to check transactions that touch only
     * their escrow (ConnectTx), and cheap however many proposals and closed bundles there are.
     */
    SidechainDB Subset(const std::set<SidechainId>& slots) const;

    /** Outpoints of the escrow outputs of the sidechains. */
    using EscrowOutputs = std::map<COutPoint, SidechainId>;
    EscrowOutputs GetEscrowOutputs() const;

    /**
     * Apply the escrow rules to a transaction that is not a coinbase: a
     * transaction that creates or spends an escrow output has to be a valid
     * deposit or withdrawal. ConnectBlock does this for every transaction of a
     * block; the mempool uses it to keep out transactions that could not be mined.
     *
     * @param[in,out] escrow_outputs   as returned by GetEscrowOutputs(), kept up to date across calls
     * @param[out]    deposit          the escrow change, if the transaction made one; may be null.
     *                                 The caller fills in the transaction and block fields.
     * @param[in]     height           of the block the transaction is in; the mempool and the miner leave
     *                                 it out, and so check by the newest rules
     * @return false if the transaction is invalid; see ConnectBlock.
     */
    [[nodiscard]] bool ConnectTx(const CTransaction& tx, const Consensus::DrivechainParams& params, EscrowOutputs& escrow_outputs,
                                 BlockUndo& undo, std::optional<Deposit>* deposit, std::string& reject_reason,
                                 int height = std::numeric_limits<int>::max());

    /** Revert the update of the block that produced `undo`. */
    void DisconnectBlock(const BlockUndo& undo);

    /** Hash of the block this state belongs to; null before any block was connected. */
    const uint256& GetBlockHash() const { return m_block_hash; }
    void SetBlockHash(const uint256& hash) { m_block_hash = hash; }

    const std::map<SidechainId, Slot>& GetSlots() const { return m_slots; }
    const Slot* GetSlot(SidechainId id) const;
    bool IsActive(SidechainId id) const { return m_slots.contains(id); }
    const std::vector<Proposal>& GetProposals() const { return m_proposals; }
    /** The proposal with this hash, for any slot; see the other overload. */
    const Proposal* GetProposal(const uint256& hash) const;
    const Proposal* GetProposal(SidechainId slot, const uint256& hash) const;
    /** Whether a bundle was paid out or failed. */
    bool IsClosed(SidechainId id, const uint256& bundle_hash) const { return m_closed.contains({id, bundle_hash}); }
    /** Whether a bundle was paid out (true) or failed (false), if it was closed. Sidechains refund the withdrawals of a failed bundle. */
    std::optional<bool> WasPaid(SidechainId id, const uint256& bundle_hash) const
    {
        const auto it{m_closed.find({id, bundle_hash})};
        return it == m_closed.end() ? std::nullopt : std::optional<bool>{it->second.paid};
    }
    /** What became of a bundle, if it is remembered as closed. */
    std::optional<ClosedBundle> GetClosed(SidechainId id, const uint256& bundle_hash) const
    {
        const auto it{m_closed.find({id, bundle_hash})};
        return it == m_closed.end() ? std::nullopt : std::optional<ClosedBundle>{it->second};
    }
    /** Number of bundles remembered as paid out or failed. */
    size_t ClosedCount() const { return m_closed.size(); }

    /**
     * The active sidechains, in slot order, with their pending bundles. The
     * vote message of the next block has one entry per element.
     */
    PendingBundles GetPendingBundles() const;
    /** What the last block voted, per sidechain; what a REPEAT_PREVIOUS vote repeats. */
    const std::map<SidechainId, Vote>& GetLastVotes() const { return m_last_votes; }

    /** Number of blocks a proposal has left before it activates, if it keeps collecting acks. */
    static int BlocksUntilActivation(const Proposal& proposal, bool slot_in_use, int height, const Consensus::DrivechainParams& params);
    /** Number of blocks without an ack a proposal had as of `height`. */
    static int Failures(const Proposal& proposal, int height);
    /** The bundle a full queue would fail to make room for a new one: the lowest score, oldest first. */
    static std::vector<Bundle>::const_iterator WeakestBundle(const std::vector<Bundle>& bundles);
    /** Number of blocks a bundle has left to reach the minimum work score, as of `height`. */
    static int BlocksLeft(const Bundle& bundle, int height, const Consensus::DrivechainParams& params);
    /**
     * Whether a bundle fails in the block at `height` before that block's votes count, for going
     * too long without an upvote (DrivechainParams::upvote_expiry_blocks).
     */
    static bool UpvoteExpired(const Bundle& bundle, int height, const Consensus::DrivechainParams& params);

    /** Hash committing to the entire state. */
    uint256 GetHash() const;

    // The state of this chain as a sidechain of another is not here: it is in its store
    // (sidechain/store.h), one entry per key, of which ConnectBlock is given an overlay.

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_block_hash << m_slots << m_proposals << m_closed << m_last_votes;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_block_hash >> m_slots >> m_proposals >> m_closed >> m_last_votes;
        m_failed_by_height.clear();
        m_unvoted_failed_by_height.clear();
        for (const auto& [key, closed] : m_closed) {
            if (!closed.paid) FailedByHeight(closed).emplace(closed.height, key.first, key.second);
        }
    }

    friend bool operator==(const SidechainDB&, const SidechainDB&) = default;

private:
    /** Remember the value of a slot before its first change in a block. */
    BlockUndo::SlotUndo& SaveSlot(SidechainId id, BlockUndo& undo) const;
    void EraseBundle(Slot& slot, SidechainId id, size_t index, BlockUndo& undo);
    /** Close a bundle; `bundle` is the pending bundle it was, if it was one (whether it was ever upvoted). */
    void CloseBundle(SidechainId id, const Bundle& bundle, bool paid, int height, BlockUndo& undo);
    using FailedSet = std::set<std::tuple<int32_t, SidechainId, uint256>>;
    FailedSet& FailedByHeight(const ClosedBundle& closed) { return closed.upvoted ? m_failed_by_height : m_unvoted_failed_by_height; }
    void RemoveProposal(size_t index, BlockUndo& undo);
    /**
     * Forget the bundles that failed withdrawal_period blocks or more before `height` (from
     * audit2_height). A failed bundle is remembered so that it is not proposed again, which a
     * sidechain that refunded its withdrawals would not expect; by then it has had a whole
     * withdrawal period to act on the failure, the bundle is older than any bundle can be while
     * pending, and paying it out again would take the same majority of the hashrate, upvoting for as
     * long, as paying out any bundle nobody vouches for.
     *
     * A failed bundle that no block ever upvoted goes after unvoted_forget_blocks. Those are what a
     * miner adds by proposing bundles nobody votes for, one per sidechain per block; remembering
     * them for a whole period would let it grow the state by that much for as long as it keeps at
     * it. Proposed again, such a bundle is where it was the first time: it starts from the same score
     * and needs every one of its votes, from miners whose sidechain node does not vouch for it (it
     * refunded it), and the record of its failure is kept for good outside the state (the closure
     * index, getsidechainevents, the miner's own record), so no node mistakes it for a new one.
     */
    void ForgetFailedBundles(int height, const Consensus::DrivechainParams& params, BlockUndo& undo);

    uint256 m_block_hash;
    //! The active sidechains.
    std::map<SidechainId, Slot> m_slots;
    //! Proposals collecting acks, oldest first.
    std::vector<Proposal> m_proposals;
    //! Bundles that were paid out or failed; they cannot be proposed again (until a failed one is forgotten).
    std::map<std::pair<SidechainId, uint256>, ClosedBundle> m_closed;
    //! The votes the last block cast: an upvote of a bundle or a downvote, per sidechain.
    std::map<SidechainId, Vote> m_last_votes;
    //! The failed bundles of m_closed by the height they failed at, oldest first; not stored, rebuilt on load.
    //! Those no block upvoted are apart, being forgotten sooner.
    FailedSet m_failed_by_height;
    FailedSet m_unvoted_failed_by_height;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_SCDB_H
