// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/state.h>

#include <addresstype.h>
#include <common/signmessage.h>
#include <crypto/common.h>
#include <drivechain/sidechain.h>
#include <key_io.h>
#include <pubkey.h>
#include <sidechain/mainchain.h>
#include <util/strencodings.h>

#include <algorithm>
#include <set>

namespace sidechain {
namespace {

/**
 * The transaction that pays `withdrawals` on the mainchain, in the blind form the mainchain votes on.
 *
 * The mainchain never votes twice on the same transaction. A bundle that pays
 * the withdrawals of one that failed has to differ from it, which the height
 * of the block that commits to it, put in the lock time, sees to. The lock
 * time has no other effect, as the mainchain spends the treasury with a final input.
 */
CMutableTransaction BuildBundle(const std::vector<const Withdrawal*>& withdrawals, int height, const uint256& nonce)
{
    // The blind form of BIP300: no inputs, the fee in place of the treasury output, then the payouts.
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = static_cast<uint32_t>(height);
    CAmount fee{0};
    for (const Withdrawal* withdrawal : withdrawals) fee += withdrawal->main_fee;
    tx.vout.emplace_back(0, drivechain::WithdrawalFeeScript(fee));
    for (const Withdrawal* withdrawal : withdrawals) tx.vout.emplace_back(withdrawal->amount, withdrawal->main_script);
    // The hash of the block before the one that commits to the bundle: nobody can know the
    // bundle's hash, and get the mainchain to close it, before the sidechain commits to it.
    tx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(nonce.begin(), nonce.end()));
    return tx;
}

} // namespace

void State::Remove(const COutPoint& outpoint, StateUndo& undo)
{
    const auto it{m_withdrawals.find(outpoint)};
    if (it == m_withdrawals.end()) return;
    undo.removed.push_back(it->second);
    m_withdrawals.erase(it);
}

bool State::InBundle(const COutPoint& withdrawal) const
{
    return m_bundle && std::find(m_bundle->withdrawals.begin(), m_bundle->withdrawals.end(), withdrawal) != m_bundle->withdrawals.end();
}

bool State::ApplyMainEvents(int main_height, const Mainchain& mainchain, int height, const Consensus::SidechainParams& params,
                            StateUndo& undo, std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    if (main_height < m_main_height) {
        reject_reason = "bad-sc-main-height";
        return false;
    }
    for (int h{m_main_height + 1}; h <= main_height; ++h) {
        const auto main_block{mainchain.GetBlock(h)};
        if (!main_block) {
            reject_reason = "bad-sc-main-unknown";
            return false;
        }
        // The bundles of this chain that the block closed.
        std::set<uint256> ours;
        for (const MainBundleEvent& event : main_block->bundles) {
            if (!m_bundle || event.hash != m_bundle->hash) continue;
            ours.insert(event.hash);
            if (event.paid) {
                for (const COutPoint& outpoint : m_bundle->withdrawals) Remove(outpoint, undo);
            } else {
                // The withdrawals wait for the next bundle, or for their owners to take them back.
                m_last_failure_height = height;
            }
            m_bundle.reset();
        }
        for (const MainDeposit& deposit : main_block->deposits) {
            if (deposit.destination == drivechain::WITHDRAWAL_RETURN_DEST) {
                // The change of a withdrawal is an escrow change too, but no deposit. If it paid a
                // bundle this chain does not have -- one committed on another branch of it -- the
                // withdrawals it paid are paid all the same: those that pay the same output with
                // the same amount, the oldest first. Without this, a branch that does not know the
                // bundle would pay its withdrawals a second time.
                if (deposit.bundle.IsNull() || ours.contains(deposit.bundle)) continue;
                for (const CTxOut& paid : deposit.payouts) {
                    // The oldest by height, then by outpoint, so that every node takes the same one.
                    auto it{m_withdrawals.end()};
                    for (auto w{m_withdrawals.begin()}; w != m_withdrawals.end(); ++w) {
                        if (w->second.main_script != paid.scriptPubKey || w->second.amount != paid.nValue) continue;
                        if (it == m_withdrawals.end() || w->second.height < it->second.height) it = w;
                    }
                    if (it == m_withdrawals.end()) continue;
                    // A bundle of ours that pays it can no longer be paid as it is.
                    if (InBundle(it->first)) m_bundle.reset();
                    Remove(it->first, undo);
                }
                continue;
            }
            if (deposit.amount <= 0) continue;
            if (!MoneyRange(deposit.amount)) {
                reject_reason = "bad-sc-deposit-amount";
                return false;
            }
            payouts.emplace_back(deposit.amount, DepositScript(deposit.destination, params.slot));
        }
    }
    m_main_height = main_height;
    return true;
}

bool State::MainPending(const Mainchain& mainchain, int height, const Consensus::SidechainParams& params) const
{
    return height >= params.single_bundle_height && mainchain.BundlePending(m_main_height);
}

bool State::MainPendingNext(const Mainchain& mainchain, int height, const Consensus::SidechainParams& params) const
{
    // A record not filled in yet may miss proposals: as if one were pending.
    return height >= params.single_bundle_height &&
           (mainchain.NeedsBackfill() || mainchain.BundlePending(m_main_height) || mainchain.BundlePending(mainchain.Height()));
}

std::optional<CMutableTransaction> State::NextBundle(int height, const uint256& prev, const Consensus::SidechainParams& params, std::vector<COutPoint>* withdrawals, bool main_pending) const
{
    if (m_bundle || m_withdrawals.empty() || main_pending) return std::nullopt;
    if (m_last_failure_height >= 0 && height - m_last_failure_height < params.bundle_retry_delay) return std::nullopt;

    // Those that offer mainchain miners the most go first; the rest wait for the next bundle.
    std::vector<const Withdrawal*> chosen;
    chosen.reserve(m_withdrawals.size());
    for (const auto& [outpoint, withdrawal] : m_withdrawals) chosen.push_back(&withdrawal);
    std::sort(chosen.begin(), chosen.end(), [](const Withdrawal* a, const Withdrawal* b) {
        if (a->main_fee != b->main_fee) return a->main_fee > b->main_fee;
        return a->outpoint < b->outpoint;
    });
    if (chosen.size() > params.max_bundle_withdrawals) chosen.resize(params.max_bundle_withdrawals);
    if (withdrawals) {
        withdrawals->clear();
        for (const Withdrawal* withdrawal : chosen) withdrawals->push_back(withdrawal->outpoint);
    }
    return BuildBundle(chosen, height, prev);
}

std::optional<CMutableTransaction> State::BundleTx() const
{
    if (!m_bundle) return std::nullopt;
    std::vector<const Withdrawal*> withdrawals;
    for (const COutPoint& outpoint : m_bundle->withdrawals) {
        const auto it{m_withdrawals.find(outpoint)};
        if (it == m_withdrawals.end()) return std::nullopt;
        withdrawals.push_back(&it->second);
    }
    return BuildBundle(withdrawals, m_bundle->height, m_bundle->nonce);
}

bool State::StartBundle(const uint256& hash, int height, const uint256& prev, const Consensus::SidechainParams& params, StateUndo& undo, std::string& reject_reason, bool main_pending)
{
    std::vector<COutPoint> withdrawals;
    const auto bundle{NextBundle(height, prev, params, &withdrawals, main_pending)};
    if (!bundle) {
        reject_reason = "bad-sc-bundle-not-allowed";
        return false;
    }
    if (bundle->GetHash().ToUint256() != hash) {
        reject_reason = "bad-sc-bundle-hash";
        return false;
    }
    m_bundle = PendingBundle{hash, std::move(withdrawals), height, prev};
    return true;
}

bool State::CheckRefund(const RefundRequest& request, std::string& reject_reason, bool main_pending) const
{
    const auto it{m_withdrawals.find(request.withdrawal)};
    if (it == m_withdrawals.end()) {
        reject_reason = "bad-sc-refund-unknown";
        return false;
    }
    if (InBundle(request.withdrawal)) {
        reject_reason = "bad-sc-refund-in-bundle";
        return false;
    }
    // A bundle pending on the mainchain may hold it, whatever this branch says: refunded and paid, it
    // would be paid twice.
    if (main_pending) {
        reject_reason = "bad-sc-refund-bundle-pending";
        return false;
    }
    CPubKey pubkey;
    if (!pubkey.RecoverCompact(MessageHash(RefundMessage(request.withdrawal)), request.signature) || !pubkey.IsCompressed() ||
        pubkey.GetID() != CKeyID{it->second.refund_keyhash}) {
        reject_reason = "bad-sc-refund-signature";
        return false;
    }
    return true;
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::SidechainParams& params, StateUndo& undo,
                    std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending)
{
    // All or nothing: a transaction that breaks a rule leaves the state as it was, so that block
    // assembly can leave it out and go on, rather than start over.
    const size_t added{undo.added.size()}, removed{undo.removed.size()}, paid{payouts.size()};
    if (ApplyTxSteps(tx, height, params, undo, payouts, reject_reason, main_pending)) return true;
    for (size_t i{added}; i < undo.added.size(); ++i) m_withdrawals.erase(undo.added[i]);
    for (size_t i{removed}; i < undo.removed.size(); ++i) m_withdrawals.emplace(undo.removed[i].outpoint, undo.removed[i]);
    undo.added.resize(added);
    undo.removed.resize(removed);
    payouts.resize(paid);
    return false;
}

bool State::ApplyTxSteps(const CTransaction& tx, int height, const Consensus::SidechainParams& params, StateUndo& undo,
                         std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending)
{
    // The chain's own rules go last, once nothing else can fail: they cannot be taken back here,
    // and fail only before they change anything. What they pay still comes first, as it always has.
    std::vector<CTxOut> refunds;
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        const CTxOut& out{tx.vout[n]};
        if (IsWithdrawalScript(out.scriptPubKey)) {
            auto withdrawal{ParseWithdrawalOutput(out)};
            if (!withdrawal) {
                reject_reason = "bad-sc-withdrawal";
                return false;
            }
            if (withdrawal->amount < params.min_withdrawal) {
                reject_reason = "bad-sc-withdrawal-amount";
                return false;
            }
            withdrawal->outpoint = COutPoint{tx.GetHash(), n};
            withdrawal->height = height;
            undo.added.push_back(withdrawal->outpoint);
            m_withdrawals.emplace(withdrawal->outpoint, std::move(*withdrawal));
        } else if (const auto refund{ParseRefundScript(out.scriptPubKey)}) {
            if (!CheckRefund(*refund, reject_reason, main_pending)) return false;
            const Withdrawal& withdrawal{m_withdrawals.at(refund->withdrawal)};
            refunds.emplace_back(withdrawal.Burned(), GetScriptForDestination(WitnessV0KeyHash{withdrawal.refund_keyhash}));
            Remove(refund->withdrawal, undo);
        }
    }
    // The shielded pool: a transaction that breaks its rules is invalid, and changes nothing.
    if (!m_shielded.ApplyTx(tx, undo.shielded, reject_reason)) return false;
    payouts.insert(payouts.end(), refunds.begin(), refunds.end());
    return true;
}

bool State::ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const Mainchain& mainchain,
                         StateUndo& undo, CAmount& minted, std::string& reject_reason)
{
    undo = StateUndo{};
    undo.main_height = m_main_height;
    undo.bundle = m_bundle;
    undo.last_failure_height = m_last_failure_height;

    // The block follows the mainchain up to the block before the one that committed to it.
    const auto bmm_height{mainchain.BmmHeight(block.GetHash())};
    if (!bmm_height) {
        reject_reason = "bmm-unknown";
        return false;
    }
    std::vector<CTxOut> payouts, tx_payouts;
    if (!ApplyMainEvents(*bmm_height - 1, mainchain, height, params, undo, payouts, reject_reason)) return false;
    const bool main_pending{MainPending(mainchain, height, params)};

    // A new bundle takes the withdrawals that were waiting before this block.
    std::optional<uint256> commitment;
    for (const CTxOut& out : block.vtx[0]->vout) {
        const auto hash{ParseBundleCommitScript(out.scriptPubKey)};
        if (!hash) continue;
        if (commitment) {
            reject_reason = "bad-sc-bundle-multiple";
            return false;
        }
        commitment = hash;
    }
    if (commitment) {
        // A bundle the mainchain closed already, before this block was committed, would stay pending for ever.
        if (const auto closed{mainchain.ClosedHeight(*commitment)}; closed && *closed < *bmm_height) {
            reject_reason = "bad-sc-bundle-closed";
            return false;
        }
        if (!StartBundle(*commitment, height, block.hashPrevBlock, params, undo, reject_reason, main_pending)) return false;
    }

    // The coinbase has no part in the shielded pool (its transactions are checked one by one below).
    if (zside::HasShieldedMarker(*block.vtx[0])) {
        reject_reason = "bad-zs-coinbase";
        return false;
    }
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        if (!ApplyTx(*block.vtx[i], height, params, undo, tx_payouts, reject_reason, main_pending)) return false;
    }

    // The root the block leaves the shielded tree with is an anchor for the next blocks.
    m_shielded.EndBlock(undo.shielded);

    // The coinbase pays the deposits and the refunds, right after its first output, as many as a block can.
    payouts = TakePayouts(std::move(payouts), std::move(tx_payouts), undo);
    const std::vector<CTxOut>& coinbase{block.vtx[0]->vout};
    if (coinbase.size() < 1 + payouts.size()) {
        reject_reason = "bad-sc-payouts-missing";
        return false;
    }
    minted = 0;
    for (size_t i{0}; i < payouts.size(); ++i) {
        if (coinbase[1 + i] != payouts[i]) {
            reject_reason = "bad-sc-payout";
            return false;
        }
        minted += payouts[i].nValue;
        if (!MoneyRange(minted)) {
            reject_reason = "bad-sc-payout-amount";
            return false;
        }
    }
    return true;
}

std::vector<CTxOut> State::TakePayouts(std::vector<CTxOut> owed, std::vector<CTxOut> owed_tx, StateUndo& undo)
{
    undo.queued = owed.size();
    undo.queued_tx = owed_tx.size();
    m_queue.insert(m_queue.end(), std::make_move_iterator(owed.begin()), std::make_move_iterator(owed.end()));
    m_queue_tx.insert(m_queue_tx.end(), std::make_move_iterator(owed_tx.begin()), std::make_move_iterator(owed_tx.end()));
    const size_t count{std::min(m_queue.size(), MAX_PAYOUTS_PER_BLOCK)};
    const size_t count_tx{std::min(m_queue_tx.size(), MAX_PAYOUTS_PER_BLOCK - count)};
    undo.paid.assign(m_queue.begin(), m_queue.begin() + count);
    undo.paid_tx.assign(m_queue_tx.begin(), m_queue_tx.begin() + count_tx);
    m_queue.erase(m_queue.begin(), m_queue.begin() + count);
    m_queue_tx.erase(m_queue_tx.begin(), m_queue_tx.begin() + count_tx);
    std::vector<CTxOut> paid{undo.paid};
    paid.insert(paid.end(), undo.paid_tx.begin(), undo.paid_tx.end());
    return paid;
}

void State::DisconnectBlock(const StateUndo& undo)
{
    // The queue was what it was before the block, with what the block owed at its end, less what
    // the block paid from its front: put the paid ones back in front, and drop the block's own.
    std::vector<CTxOut> queue{undo.paid};
    queue.insert(queue.end(), m_queue.begin(), m_queue.end());
    queue.resize(queue.size() - undo.queued);
    m_queue = std::move(queue);
    std::vector<CTxOut> queue_tx{undo.paid_tx};
    queue_tx.insert(queue_tx.end(), m_queue_tx.begin(), m_queue_tx.end());
    queue_tx.resize(queue_tx.size() - undo.queued_tx);
    m_queue_tx = std::move(queue_tx);
    // A withdrawal that the block both added and removed ends up removed.
    for (const Withdrawal& withdrawal : undo.removed) m_withdrawals.emplace(withdrawal.outpoint, withdrawal);
    for (const COutPoint& outpoint : undo.added) m_withdrawals.erase(outpoint);
    m_main_height = undo.main_height;
    m_bundle = undo.bundle;
    m_last_failure_height = undo.last_failure_height;
    m_shielded.Revert(undo.shielded);
}

} // namespace sidechain
