// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_STATE_H
#define BITCOIN_SIDECHAIN_STATE_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <zside/shielded.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * The rules that make this chain a sidechain.
 *
 * Coins enter the chain when the mainchain records a deposit to it, and leave
 * it when the mainchain pays out a withdrawal bundle:
 *
 *  - Blind merged mining. A block is valid only if a mainchain block committed
 *    to its hash. A block follows the mainchain up to the block before the one
 *    that committed to it, and has to act on everything the mainchain did for
 *    this sidechain since its parent block.
 *  - Deposits. The coinbase pays every deposit the mainchain recorded to the
 *    destination the depositor named.
 *  - Withdrawals. A transaction burns coins in a withdrawal output, which
 *    names a mainchain destination and a fee for mainchain miners.
 *  - Bundles. A block may commit to a withdrawal bundle: the mainchain
 *    transaction that pays the waiting withdrawals. Which withdrawals go in a
 *    bundle is fixed by the rules, so every node builds the same one. There is
 *    at most one bundle at a time; the mainchain votes on it, then pays it or
 *    lets it fail, and a block acts on that outcome.
 *  - Refunds. Whoever made a withdrawal can take it back while it is not in a
 *    bundle; the coinbase of the block with the request pays the coins back.
 */
namespace sidechain {

inline constexpr unsigned char WITHDRAWAL_TAG[4]{0xD8, 0x57, 0x44, 0x52};
inline constexpr unsigned char REFUND_TAG[4]{0xD9, 0x52, 0x46, 0x4E};
inline constexpr unsigned char BUNDLE_COMMIT_TAG[4]{0xDA, 0x42, 0x4E, 0x44};

/** Largest mainchain output script a withdrawal can pay to, in bytes. */
inline constexpr size_t MAX_MAIN_SCRIPT_SIZE{80};
/**
 * Most outputs a coinbase pays for deposits, refunds and the like; what is owed
 * beyond waits, in order, for the next blocks. Without a limit, enough deposits
 * at once would make every block too large to be valid.
 */
inline constexpr size_t MAX_PAYOUTS_PER_BLOCK{1000};
/**
 * Blocks during which a new withdrawal can still be taken back ahead of a bundle that would take it
 * (block assembly policy, not a rule): after that, a bundle goes before refund requests.
 */
inline constexpr int REFUND_GRACE_BLOCKS{6};
/**
 * A bundle of another branch that leads the slot on the mainchain holds refunds back whatever its
 * score while it rose by PENDING_TREND_MIN_RISE or more over the last PENDING_TREND_BLOCKS mainchain
 * blocks (see SidechainParams::pending_min_score): net upvotes in a quarter of the
 * blocks, half the pace at which a bundle reaches the mainchain's withdrawal_min_score in its
 * withdrawal_period. Slower, it is on its way to failing; keeping it at that pace against the
 * downvotes of the miners who vouch for this chain takes a good share of the hashrate, block after block.
 */
inline constexpr int PENDING_TREND_BLOCKS{12};
inline constexpr uint32_t PENDING_TREND_MIN_RISE{3};

/** Coins burned on this chain, to be paid out on the mainchain. */
struct Withdrawal {
    //! The output that burned the coins. It identifies the withdrawal.
    COutPoint outpoint;
    //! Amount to pay on the mainchain.
    CAmount amount{0};
    //! Fee offered to mainchain miners for paying the withdrawal.
    CAmount main_fee{0};
    //! Mainchain output script to pay.
    CScript main_script;
    //! Hash of the key that can take the withdrawal back; a refund pays its P2WPKH address.
    uint160 refund_keyhash;
    //! Height of the block that contains the withdrawal.
    int32_t height{0};

    /** The coins the withdrawal took out of circulation on this chain. */
    CAmount Burned() const { return amount + main_fee; }

    SERIALIZE_METHODS(Withdrawal, obj) { READWRITE(obj.outpoint, obj.amount, obj.main_fee, obj.main_script, obj.refund_keyhash, obj.height); }
    friend bool operator==(const Withdrawal&, const Withdrawal&) = default;
};

/** The withdrawal bundle the mainchain is voting on. */
struct PendingBundle {
    //! Hash the mainchain votes on: that of the bundle transaction.
    uint256 hash;
    //! The withdrawals the bundle pays, in the order of its outputs.
    std::vector<COutPoint> withdrawals;
    //! Height of the block that committed to the bundle.
    int32_t height{0};
    //! Hash of the block before that one, which the bundle carries so that its hash cannot be known in advance.
    uint256 nonce;

    SERIALIZE_METHODS(PendingBundle, obj) { READWRITE(obj.hash, obj.withdrawals, obj.height, obj.nonce); }
    friend bool operator==(const PendingBundle&, const PendingBundle&) = default;
};

/** A request to take a withdrawal back, found in a transaction output. */
struct RefundRequest {
    COutPoint withdrawal;
    //! Signature of RefundMessage() by the refund key of the withdrawal, in the format of signed messages.
    std::vector<unsigned char> signature;
};

class Mainchain;

/**
 * The sidechain state implied by a chain of blocks and the record of the mainchain, read from a
 * store (sidechain/store.h) and, when given an overlay, written to it. It holds nothing itself:
 * a State is a way to look at a store, as cheap to make as a pointer.
 *
 * Tables (the first byte of the keys): 'w' withdrawals by outpoint; 'f' the same by fee (highest
 * first), 'p' by what they pay (oldest first): indexes, so that no rule reads every withdrawal;
 * 'q' and 'r' the payout queues, by position; single values under 'h', 'b', 'x', 'Q', 'R' (heights,
 * the pending bundle, where each queue starts and ends). Sidechains add tables of their own, under
 * other bytes (see SIDECHAIN_TABLES).
 */
class State
{
public:
    /** Read only. */
    explicit State(const StoreView& view) : m_view{&view}, m_overlay{nullptr} {}
    /** Read and write. */
    explicit State(StoreOverlay& overlay) : m_view{&overlay}, m_overlay{&overlay} {}

    /**
     * Check `block`, to be connected at `height` on top of the block this
     * state belongs to, against the sidechain rules, and update the state.
     * What it changes, the overlay's journal notes: the undo data of the block.
     *
     * @param[out] minted         the coins the coinbase has to create on top of the fees: deposits and refunds
     * @param[out] reject_reason  set when the block is invalid
     * @return false if the block is invalid, in which case the overlay holds a partial update and must be dropped.
     */
    [[nodiscard]] bool ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const Mainchain& mainchain,
                                    CAmount& minted, std::string& reject_reason);

    //
    // The steps of ConnectBlock, in the order it takes them. Block assembly
    // takes the same steps to learn what a block has to contain.
    //

    /**
     * Follow the mainchain up to `main_height`: note the outcome of the
     * pending bundle, and collect the outputs that pay the deposits.
     * @param[in,out] payouts  outputs the coinbase must have, in order, after its first output
     */
    [[nodiscard]] bool ApplyMainEvents(int main_height, const Mainchain& mainchain, int height, const Consensus::SidechainParams& params,
                                       std::vector<CTxOut>& payouts, std::string& reject_reason);
    /**
     * The bundle a block at `height`, on top of the block `prev`, may commit to: empty if no bundle can
     * be made now. A bundle of another branch pending on the mainchain does not hold it back.
     */
    std::optional<CMutableTransaction> NextBundle(int height, const uint256& prev, const Consensus::SidechainParams& params, std::vector<COutPoint>* withdrawals = nullptr) const;
    /** Make the bundle with the hash `hash` the pending one. */
    [[nodiscard]] bool StartBundle(const uint256& hash, int height, const uint256& prev, const Consensus::SidechainParams& params, std::string& reject_reason);
    /**
     * Whether a bundle of this sidechain other than its own, pending on the mainchain with support
     * (see SidechainParams::pending_min_score), holds refunds back for a block with this state: as
     * the mainchain was after the last block this state acted on.
     */
    bool MainPending(const Mainchain& mainchain, const Consensus::SidechainParams& params) const;
    /**
     * The same as the next block will see it: it follows the mainchain up to the last block on record
     * first. For the mempool, which must not take what that block would refuse.
     */
    bool MainPendingNext(const Mainchain& mainchain, const Consensus::SidechainParams& params) const;
    /**
     * The payouts a block pays: at most MAX_PAYOUTS_PER_BLOCK, of what was owed
     * before it and of what it gave rise to, oldest first in each queue. What
     * the mainchain gave rise to (`owed`: deposits) has a queue of its own, apart
     * from what transactions did (`owed_tx`: refunds, and what a sidechain's own
     * rules pay out), so that transactions, however many, cannot hold deposits
     * back. Each queue has half of the block, and what one does not use goes to
     * the other, so that neither holds the other back. The rest is queued for
     * the next blocks.
     */
    std::vector<CTxOut> TakePayouts(std::vector<CTxOut> owed, std::vector<CTxOut> owed_tx);
    /** Payouts owed and not paid yet, oldest first: from the mainchain, and from transactions. */
    std::vector<CTxOut> Queue() const;
    std::vector<CTxOut> TxQueue() const;
    /**
     * Take in the withdrawals and the refund requests of a transaction that is not a coinbase.
     * All or nothing: a transaction that breaks a rule changes nothing.
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, const Consensus::SidechainParams& params,
                               std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending = false);

    /** Whether a refund request could be mined now. @param[in] main_pending  see MainPending */
    [[nodiscard]] bool CheckRefund(const RefundRequest& request, std::string& reject_reason, bool main_pending = false) const;

    /** Height of the last mainchain block this state has acted on. */
    int32_t MainHeight() const;
    /** That height before a block, from its undo data and the height after it. */
    static int32_t MainHeightBefore(const StoreUndo& undo, int32_t after);
    std::optional<Withdrawal> GetWithdrawal(const COutPoint& outpoint) const;
    /** The withdrawals not paid yet, by outpoint, until `fn` returns false. */
    void ForEachWithdrawal(const std::function<bool(const Withdrawal&)>& fn) const;
    std::optional<PendingBundle> Bundle() const;
    /** The transaction of the pending bundle, as the mainchain wants to receive it. */
    std::optional<CMutableTransaction> BundleTx() const;
    bool InBundle(const COutPoint& withdrawal) const;
    int32_t LastFailureHeight() const;
    /** Height of the mainchain block that committed to the pending bundle; -1 if none. */
    int32_t BundleMainHeight() const;

    /** The shielded pool, read only. */
    zside::ShieldedState Shielded() const { return zside::ShieldedState{*m_view}; }

    /** A hash of the whole state: the same on every node with the same chain. */
    uint256 Hash() const { return StoreHash(*m_view); }

    const StoreView& View() const { return *m_view; }

protected:
    /** The overlay to write to; a State made read only has none. */
    StoreOverlay& Writable() const;
    /** ApplyTx, on an overlay of its own. */
    [[nodiscard]] bool ApplyTxSteps(const CTransaction& tx, int height, const Consensus::SidechainParams& params,
                                    std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending);

private:
    void AddWithdrawal(const Withdrawal& withdrawal);
    void Remove(const COutPoint& withdrawal);
    void SetBundle(const std::optional<PendingBundle>& bundle);

    const StoreView* m_view;
    StoreOverlay* m_overlay;
};

//
// Script formats. Each is an OP_RETURN output with a single data push that
// starts with a four byte tag.
//

/**
 * Output that burns its value as a withdrawal:
 * WITHDRAWAL_TAG | fee for mainchain miners (8 bytes) | refund key hash (20 bytes) | mainchain output script.
 * The amount paid on the mainchain is the value of the output less the fee.
 */
CScript WithdrawalScript(CAmount main_fee, const uint160& refund_keyhash, const CScript& main_script);
/** If the output is a withdrawal, return it; its outpoint and height are left for the caller to fill in. */
std::optional<Withdrawal> ParseWithdrawalOutput(const CTxOut& out);
/** Whether the script claims to be a withdrawal, well formed or not. */
bool IsWithdrawalScript(const CScript& script);

/** REFUND_TAG | txid (32 bytes) | output index (4 bytes) | signature (65 bytes) */
CScript RefundScript(const RefundRequest& request);
std::optional<RefundRequest> ParseRefundScript(const CScript& script);
/** The message the refund key signs to take a withdrawal back. */
std::string RefundMessage(const COutPoint& withdrawal);

/**
 * Whether the mempool checks of the transaction look at what the mempool holds of the sidechain
 * state: a refund request (one per withdrawal in the mempool), and whatever else a sidechain built
 * on this one keeps there (add it here). Such a transaction is never accepted as part of a package,
 * whose members are checked before any of them is in the mempool and would not see each other: it
 * comes on its own, paying its own way.
 */
bool CheckedAgainstMempool(const CTransaction& tx);

/** Coinbase output committing to a withdrawal bundle: BUNDLE_COMMIT_TAG | bundle hash */
CScript BundleCommitScript(const uint256& hash);
std::optional<uint256> ParseBundleCommitScript(const CScript& script);

/**
 * Whether a block that failed for `reject_reason` (from State::ConnectBlock) fails whatever the
 * record of the mainchain says. Unknown reasons, those of a sidechain's own rules included, are
 * taken to depend on it.
 */
bool RecordIndependent(const std::string& reject_reason);

/**
 * Script that pays a deposit. The depositor names a deposit address of this
 * sidechain, s<slot>_<address>_<checksum>, or an address of this chain as it
 * is. Anything else, a deposit address for another sidechain or one with a
 * wrong checksum included, burns the coins.
 */
CScript DepositScript(const std::string& destination, uint32_t slot);

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_STATE_H
