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
#include <crypto/sha256.h>
#include <util/check.h>
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


//
// Tables of the state.
//

const Table<COutPoint, Withdrawal> WITHDRAWALS{'w'};
constexpr uint8_t BY_FEE{'f'};
constexpr uint8_t BY_PAYOUT{'p'};
const Table<uint64_t, CTxOut> QUEUE{'q'};
const Table<uint64_t, CTxOut> TX_QUEUE{'r'};

struct QueueEnds {
    uint64_t head{0};
    uint64_t tail{0};
    SERIALIZE_METHODS(QueueEnds, obj) { READWRITE(obj.head, obj.tail); }
    friend bool operator==(const QueueEnds&, const QueueEnds&) = default;
};
const Cell<QueueEnds> QUEUE_ENDS{'Q', {}};
const Cell<QueueEnds> TX_QUEUE_ENDS{'R', {}};

struct BundleCell {
    std::optional<PendingBundle> bundle;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << bundle.has_value();
        if (bundle) s << *bundle;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bool has;
        s >> has;
        bundle.reset();
        if (has) s >> bundle.emplace();
    }
    friend bool operator==(const BundleCell&, const BundleCell&) = default;
};
const Cell<BundleCell> BUNDLE{'b', {}};
const Cell<int32_t> MAIN_HEIGHT{'h', -1};
const Cell<int32_t> LAST_FAILURE{'x', -1};

/** Signed integers in key order. */
void EncodeSigned(StoreBytes& out, int64_t v) { KeyCodec<uint64_t>::Encode(out, static_cast<uint64_t>(v) ^ (uint64_t{1} << 63)); }
void EncodeSigned(StoreBytes& out, int32_t v) { KeyCodec<uint32_t>::Encode(out, static_cast<uint32_t>(v) ^ (uint32_t{1} << 31)); }

/** Highest fee first, then by outpoint: the order bundles take withdrawals in. */
StoreBytes FeeKey(const Withdrawal& w)
{
    StoreBytes key{BY_FEE};
    KeyCodec<uint64_t>::Encode(key, ~(static_cast<uint64_t>(w.main_fee) ^ (uint64_t{1} << 63)));
    KeyCodec<COutPoint>::Encode(key, w.outpoint);
    return key;
}

/** What a withdrawal pays on the mainchain; within it, the oldest first, then by outpoint. */
StoreBytes PayoutPrefix(const CScript& script, CAmount amount)
{
    StoreBytes key{BY_PAYOUT};
    uint256 script_hash;
    CSHA256().Write(script.data(), script.size()).Finalize(script_hash.begin());
    KeyCodec<uint256>::Encode(key, script_hash);
    EncodeSigned(key, int64_t{amount});
    return key;
}
StoreBytes PayoutKey(const Withdrawal& w)
{
    StoreBytes key{PayoutPrefix(w.main_script, w.amount)};
    EncodeSigned(key, w.height);
    KeyCodec<COutPoint>::Encode(key, w.outpoint);
    return key;
}
/** The outpoint at the end of an index key. */
COutPoint OutpointOf(const StoreBytes& key)
{
    std::span<const unsigned char> rest{key};
    rest = rest.subspan(key.size() - 36);
    return KeyCodec<COutPoint>::Decode(rest);
}

std::vector<CTxOut> ReadQueue(const StoreView& view, const Table<uint64_t, CTxOut>& table, const Cell<QueueEnds>& ends)
{
    std::vector<CTxOut> out;
    table.ForEach(view, [&](const uint64_t&, const CTxOut& o) {
        out.push_back(o);
        return true;
    }, ends.Get(view).head);
    return out;
}

} // namespace

StoreOverlay& State::Writable() const
{
    assert(m_overlay);
    return *m_overlay;
}

int32_t State::MainHeight() const { return MAIN_HEIGHT.Get(*m_view); }
int32_t State::MainHeightBefore(const StoreUndo& undo, int32_t after)
{
    const StoreBytes key{'h'};
    for (const auto& [k, value] : undo.entries) {
        if (k == key) return value ? DecodeValue<int32_t>(*value) : -1;
    }
    return after;
}

int32_t State::LastFailureHeight() const { return LAST_FAILURE.Get(*m_view); }
std::optional<PendingBundle> State::Bundle() const { return BUNDLE.Get(*m_view).bundle; }
std::optional<Withdrawal> State::GetWithdrawal(const COutPoint& outpoint) const { return WITHDRAWALS.Get(*m_view, outpoint); }
std::vector<CTxOut> State::Queue() const { return ReadQueue(*m_view, QUEUE, QUEUE_ENDS); }
std::vector<CTxOut> State::TxQueue() const { return ReadQueue(*m_view, TX_QUEUE, TX_QUEUE_ENDS); }

void State::ForEachWithdrawal(const std::function<bool(const Withdrawal&)>& fn) const
{
    WITHDRAWALS.ForEach(*m_view, [&](const COutPoint&, const Withdrawal& w) { return fn(w); });
}

void State::SetBundle(const std::optional<PendingBundle>& bundle) { BUNDLE.Put(Writable(), BundleCell{bundle}); }

void State::AddWithdrawal(const Withdrawal& withdrawal)
{
    StoreOverlay& out{Writable()};
    WITHDRAWALS.Put(out, withdrawal.outpoint, withdrawal);
    out.Put(FeeKey(withdrawal), {});
    out.Put(PayoutKey(withdrawal), {});
}

void State::Remove(const COutPoint& outpoint)
{
    const auto withdrawal{GetWithdrawal(outpoint)};
    if (!withdrawal) return;
    StoreOverlay& out{Writable()};
    WITHDRAWALS.Erase(out, outpoint);
    out.Erase(FeeKey(*withdrawal));
    out.Erase(PayoutKey(*withdrawal));
}

bool State::InBundle(const COutPoint& withdrawal) const
{
    const auto bundle{Bundle()};
    return bundle && std::find(bundle->withdrawals.begin(), bundle->withdrawals.end(), withdrawal) != bundle->withdrawals.end();
}

bool State::ApplyMainEvents(int main_height, const Mainchain& mainchain, int height, const Consensus::SidechainParams& params,
                            std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    const int32_t from{MainHeight()};
    if (main_height < from) {
        reject_reason = "bad-sc-main-height";
        return false;
    }
    for (int h{from + 1}; h <= main_height; ++h) {
        const auto main_block{mainchain.GetBlock(h)};
        if (!main_block) {
            reject_reason = "bad-sc-main-unknown";
            return false;
        }
        // The bundles of this chain that the block closed.
        std::set<uint256> ours;
        for (const MainBundleEvent& event : main_block->bundles) {
            const auto bundle{Bundle()};
            if (!bundle || event.hash != bundle->hash) continue;
            ours.insert(event.hash);
            if (event.paid) {
                for (const COutPoint& outpoint : bundle->withdrawals) Remove(outpoint);
            } else {
                // The withdrawals wait for the next bundle, or for their owners to take them back.
                LAST_FAILURE.Put(Writable(), height);
            }
            SetBundle(std::nullopt);
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
                    // The oldest by height, then by outpoint, so that every node takes the same one:
                    // the first in the index of what withdrawals pay.
                    const StoreBytes prefix{PayoutPrefix(paid.scriptPubKey, paid.nValue)};
                    const auto first{m_view->Next(prefix, prefix)};
                    if (!first) continue;
                    const COutPoint outpoint{OutpointOf(first->first)};
                    // A bundle of ours that pays it can no longer be paid as it is.
                    if (InBundle(outpoint)) SetBundle(std::nullopt);
                    Remove(outpoint);
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
    MAIN_HEIGHT.Put(Writable(), main_height);
    return true;
}

bool State::MainPending(const Mainchain& mainchain, int height, const Consensus::SidechainParams& params) const
{
    return height >= params.single_bundle_height && mainchain.BundlePending(MainHeight());
}

bool State::MainPendingNext(const Mainchain& mainchain, int height, const Consensus::SidechainParams& params) const
{
    // A record not filled in yet may miss proposals: as if one were pending.
    return height >= params.single_bundle_height &&
           (mainchain.NeedsBackfill() || mainchain.BundlePending(MainHeight()) || mainchain.BundlePending(mainchain.Height()));
}

std::optional<CMutableTransaction> State::NextBundle(int height, const uint256& prev, const Consensus::SidechainParams& params, std::vector<COutPoint>* withdrawals, bool main_pending) const
{
    if (Bundle() || main_pending) return std::nullopt;
    const int32_t last_failure{LastFailureHeight()};
    if (last_failure >= 0 && height - last_failure < params.bundle_retry_delay) return std::nullopt;

    // Those that offer mainchain miners the most go first; the rest wait for the next bundle. The
    // fee index has them in that order (then by outpoint): its first entries are the bundle.
    std::vector<Withdrawal> chosen;
    const StoreBytes prefix{BY_FEE};
    StoreBytes at{prefix};
    while (chosen.size() < params.max_bundle_withdrawals) {
        const auto entry{m_view->Next(at, prefix)};
        if (!entry) break;
        chosen.push_back(*Assert(GetWithdrawal(OutpointOf(entry->first))));
        at = entry->first;
        at.push_back(0);
    }
    if (chosen.empty()) return std::nullopt;
    if (withdrawals) {
        withdrawals->clear();
        for (const Withdrawal& withdrawal : chosen) withdrawals->push_back(withdrawal.outpoint);
    }
    std::vector<const Withdrawal*> pointers;
    for (const Withdrawal& withdrawal : chosen) pointers.push_back(&withdrawal);
    return BuildBundle(pointers, height, prev);
}

std::optional<CMutableTransaction> State::BundleTx() const
{
    const auto bundle{Bundle()};
    if (!bundle) return std::nullopt;
    std::vector<Withdrawal> withdrawals;
    for (const COutPoint& outpoint : bundle->withdrawals) {
        auto withdrawal{GetWithdrawal(outpoint)};
        if (!withdrawal) return std::nullopt;
        withdrawals.push_back(std::move(*withdrawal));
    }
    std::vector<const Withdrawal*> pointers;
    for (const Withdrawal& withdrawal : withdrawals) pointers.push_back(&withdrawal);
    return BuildBundle(pointers, bundle->height, bundle->nonce);
}

bool State::StartBundle(const uint256& hash, int height, const uint256& prev, const Consensus::SidechainParams& params, std::string& reject_reason, bool main_pending)
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
    SetBundle(PendingBundle{hash, std::move(withdrawals), height, prev});
    return true;
}

bool State::CheckRefund(const RefundRequest& request, std::string& reject_reason, bool main_pending) const
{
    const auto withdrawal{GetWithdrawal(request.withdrawal)};
    if (!withdrawal) {
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
        pubkey.GetID() != CKeyID{withdrawal->refund_keyhash}) {
        reject_reason = "bad-sc-refund-signature";
        return false;
    }
    return true;
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::SidechainParams& params,
                    std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending)
{
    // All or nothing: on an overlay of its own, kept only if the transaction keeps the rules, so that
    // block assembly can leave a transaction out and go on, rather than start over.
    StoreOverlay tx_overlay{Writable(), /*journal=*/false};
    const size_t paid{payouts.size()};
    State tx_state{tx_overlay};
    if (!tx_state.ApplyTxSteps(tx, height, params, payouts, reject_reason, main_pending)) {
        payouts.resize(paid);
        return false;
    }
    tx_overlay.MergeInto(Writable());
    return true;
}

bool State::ApplyTxSteps(const CTransaction& tx, int height, const Consensus::SidechainParams& params,
                         std::vector<CTxOut>& payouts, std::string& reject_reason, bool main_pending)
{
    // The shielded pool goes last, once nothing else can fail; what it pays (nothing, so far)
    // would still come first.
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
            AddWithdrawal(*withdrawal);
        } else if (const auto refund{ParseRefundScript(out.scriptPubKey)}) {
            if (!CheckRefund(*refund, reject_reason, main_pending)) return false;
            const Withdrawal withdrawal{*Assert(GetWithdrawal(refund->withdrawal))};
            refunds.emplace_back(withdrawal.Burned(), GetScriptForDestination(WitnessV0KeyHash{withdrawal.refund_keyhash}));
            Remove(refund->withdrawal);
        }
    }
    // The shielded pool: a transaction that breaks its rules is invalid.
    zside::ShieldedState shielded{Writable()};
    if (!shielded.ApplyTx(tx, reject_reason)) return false;
    payouts.insert(payouts.end(), refunds.begin(), refunds.end());
    return true;
}

bool State::ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const Mainchain& mainchain,
                         CAmount& minted, std::string& reject_reason)
{
    // The block follows the mainchain up to the block before the one that committed to it.
    const auto bmm_height{mainchain.BmmHeight(block.GetHash())};
    if (!bmm_height) {
        reject_reason = "bmm-unknown";
        return false;
    }
    std::vector<CTxOut> payouts, tx_payouts;
    if (!ApplyMainEvents(*bmm_height - 1, mainchain, height, params, payouts, reject_reason)) return false;
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
        if (!StartBundle(*commitment, height, block.hashPrevBlock, params, reject_reason, main_pending)) return false;
    }

    // The coinbase has no part in the shielded pool (its transactions are checked one by one below).
    if (zside::HasShieldedMarker(*block.vtx[0])) {
        reject_reason = "bad-zs-coinbase";
        return false;
    }
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        if (!ApplyTx(*block.vtx[i], height, params, tx_payouts, reject_reason, main_pending)) return false;
    }

    // The root the block leaves the shielded tree with is an anchor for the next blocks.
    zside::ShieldedState{Writable()}.EndBlock();

    // The coinbase pays the deposits and the refunds, right after its first output, as many as a block can.
    payouts = TakePayouts(std::move(payouts), std::move(tx_payouts));
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

std::vector<CTxOut> State::TakePayouts(std::vector<CTxOut> owed, std::vector<CTxOut> owed_tx)
{
    StoreOverlay& out{Writable()};
    // Each queue: what is owed goes at its end; what is paid comes from its front.
    const auto push{[&](const Table<uint64_t, CTxOut>& table, const Cell<QueueEnds>& cell, const std::vector<CTxOut>& owed_now) {
        QueueEnds ends{cell.Get(*m_view)};
        for (const CTxOut& o : owed_now) table.Put(out, ends.tail++, o);
        cell.Put(out, ends);
        return ends.tail - ends.head;
    }};
    const auto take{[&](const Table<uint64_t, CTxOut>& table, const Cell<QueueEnds>& cell, uint64_t count, std::vector<CTxOut>& paid) {
        QueueEnds ends{cell.Get(*m_view)};
        for (uint64_t i{0}; i < count; ++i) {
            paid.push_back(*Assert(table.Get(*m_view, ends.head)));
            table.Erase(out, ends.head++);
        }
        // An empty queue starts again at 0, so that a state never holds a count that only grows.
        if (ends.head == ends.tail) ends = QueueEnds{};
        cell.Put(out, ends);
    }};
    const uint64_t queued{push(QUEUE, QUEUE_ENDS, owed)};
    const uint64_t queued_tx{push(TX_QUEUE, TX_QUEUE_ENDS, owed_tx)};
    const uint64_t count{std::min<uint64_t>(queued, MAX_PAYOUTS_PER_BLOCK)};
    const uint64_t count_tx{std::min<uint64_t>(queued_tx, MAX_PAYOUTS_PER_BLOCK - count)};
    std::vector<CTxOut> paid;
    take(QUEUE, QUEUE_ENDS, count, paid);
    take(TX_QUEUE, TX_QUEUE_ENDS, count_tx, paid);
    return paid;
}

} // namespace sidechain
