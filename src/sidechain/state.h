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
#include <uint256.h>
#include <zside/shielded.h>

#include <cstdint>
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

/** What a block changed in the State, in the form needed to revert it. */
struct StateUndo {
    int32_t main_height{-1};
    std::optional<PendingBundle> bundle;
    int32_t last_failure_height{-1};
    //! Withdrawals the block added.
    std::vector<COutPoint> added;
    //! Withdrawals the block removed, by paying or refunding them.
    std::vector<Withdrawal> removed;
    //! Payouts the block took from the front of each queue, and how many it added at their ends.
    std::vector<CTxOut> paid;
    uint64_t queued{0};
    std::vector<CTxOut> paid_tx;
    uint64_t queued_tx{0};
    //! What the block changed in the shielded pool.
    zside::ShieldedUndo shielded;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << main_height << bundle.has_value();
        if (bundle) s << *bundle;
        s << last_failure_height << added << removed << paid << queued << paid_tx << queued_tx << shielded;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bool has_bundle;
        s >> main_height >> has_bundle;
        bundle.reset();
        if (has_bundle) s >> bundle.emplace();
        s >> last_failure_height >> added >> removed >> paid >> queued >> paid_tx >> queued_tx >> shielded;
    }
};

class Mainchain;

/** The sidechain state implied by a chain of blocks and the record of the mainchain. */
class State
{
public:
    /**
     * Check `block`, to be connected at `height` on top of the block this
     * state belongs to, against the sidechain rules, and update the state.
     *
     * @param[out] undo           what is needed to revert the update
     * @param[out] minted         the coins the coinbase has to create on top of the fees: deposits and refunds
     * @param[out] reject_reason  set when the block is invalid
     * @return false if the block is invalid, in which case the state is left
     *         partially updated and must be discarded.
     */
    [[nodiscard]] bool ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const Mainchain& mainchain,
                                    StateUndo& undo, CAmount& minted, std::string& reject_reason);
    /** Revert the update of the block that produced `undo`. */
    void DisconnectBlock(const StateUndo& undo);

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
                                       StateUndo& undo, std::vector<CTxOut>& payouts, std::string& reject_reason);
    /**
     * The bundle a block at `height`, on top of the block `prev`, may commit to: empty if no bundle can be made now.
     * @param[in] main_pending  whether a bundle of this sidechain is pending on the mainchain (MainPending)
     */
    std::optional<CMutableTransaction> NextBundle(int height, const uint256& prev, const Consensus::SidechainParams& params, std::vector<COutPoint>* withdrawals = nullptr, bool main_pending = false) const;
    /** Make the bundle with the hash `hash` the pending one. */
    [[nodiscard]] bool StartBundle(const uint256& hash, int height, const uint256& prev, const Consensus::SidechainParams& params, StateUndo& undo, std::string& reject_reason, bool main_pending = false);
    /**
     * Whether a block at `height`, with this state, has to wait for a bundle of this sidechain that is
     * pending on the mainchain (SidechainParams::single_bundle_height): as the mainchain was after
     * the last block this state acted on.
     */
    bool MainPending(const Mainchain& mainchain, int height, const Consensus::SidechainParams& params) const;
    /**
     * The same as the next block will see it: it follows the mainchain up to the last block on record
     * first. For the mempool, which must not take what that block would refuse.
     */
    bool MainPendingNext(const Mainchain& mainchain, int height, const Consensus::SidechainParams& params) const;
    /**
     * The payouts a block pays: at most MAX_PAYOUTS_PER_BLOCK, of what was owed
     * before it and of what it gave rise to, oldest first. What the mainchain
     * gave rise to (`owed`: deposits) goes before what transactions did
     * (`owed_tx`: refunds and the like), so that transactions, however many,
     * cannot hold deposits back. The rest is queued for the next blocks.
     */
    std::vector<CTxOut> TakePayouts(std::vector<CTxOut> owed, std::vector<CTxOut> owed_tx, StateUndo& undo);
    /** Payouts owed and not paid yet, oldest first: from the mainchain, and from transactions. */
    const std::vector<CTxOut>& Queue() const { return m_queue; }
    const std::vector<CTxOut>& TxQueue() const { return m_queue_tx; }
    /** Take in the withdrawals and the refund requests of a transaction that is not a coinbase. */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, const Consensus::SidechainParams& params, StateUndo& undo,
                               std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending = false);

    /** Whether a refund request could be mined now. @param[in] main_pending  see MainPending */
    [[nodiscard]] bool CheckRefund(const RefundRequest& request, std::string& reject_reason, bool main_pending = false) const;

    /** Height of the last mainchain block this state has acted on. */
    int32_t MainHeight() const { return m_main_height; }
    const std::map<COutPoint, Withdrawal>& Withdrawals() const { return m_withdrawals; }
    const std::optional<PendingBundle>& Bundle() const { return m_bundle; }
    /** The transaction of the pending bundle, as the mainchain wants to receive it. */
    std::optional<CMutableTransaction> BundleTx() const;
    bool InBundle(const COutPoint& withdrawal) const;
    int32_t LastFailureHeight() const { return m_last_failure_height; }
    /** The shielded pool. */
    const zside::ShieldedState& Shielded() const { return m_shielded; }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_main_height << m_withdrawals << m_bundle.has_value();
        if (m_bundle) s << *m_bundle;
        s << m_last_failure_height << m_queue << m_queue_tx << m_shielded;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bool has_bundle;
        s >> m_main_height >> m_withdrawals >> has_bundle;
        m_bundle.reset();
        if (has_bundle) s >> m_bundle.emplace();
        s >> m_last_failure_height >> m_queue >> m_queue_tx >> m_shielded;
    }
    friend bool operator==(const State&, const State&) = default;

private:
    void Remove(const COutPoint& withdrawal, StateUndo& undo);

    int32_t m_main_height{-1};
    //! Withdrawals that the mainchain has not paid yet: those waiting for a bundle and those in the pending bundle.
    std::map<COutPoint, Withdrawal> m_withdrawals;
    std::optional<PendingBundle> m_bundle;
    //! Height of the block that learned that a bundle failed, -1 if none did.
    int32_t m_last_failure_height{-1};
    //! Payouts owed beyond what earlier blocks could pay, oldest first: from the mainchain, and from transactions.
    std::vector<CTxOut> m_queue;
    std::vector<CTxOut> m_queue_tx;
    zside::ShieldedState m_shielded;
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

/** Coinbase output committing to a withdrawal bundle: BUNDLE_COMMIT_TAG | bundle hash */
CScript BundleCommitScript(const uint256& hash);
std::optional<uint256> ParseBundleCommitScript(const CScript& script);

/**
 * Script that pays a deposit. The depositor names a deposit address of this
 * sidechain, s<slot>_<address>_<checksum>, or an address of this chain as it
 * is. Anything else, a deposit address for another sidechain or one with a
 * wrong checksum included, burns the coins.
 */
CScript DepositScript(const std::string& destination, uint32_t slot);

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_STATE_H
