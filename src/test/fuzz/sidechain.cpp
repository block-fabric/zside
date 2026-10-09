// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The sidechain state under fuzzing: withdrawal and refund scripts read back what they were made
// from, and the state, fed withdrawals, refunds and the events of a fuzzed mainchain, comes back
// exactly to where it was when a block is undone. No refund ever goes through while a bundle of
// the sidechain is pending on the mainchain.

#include <consensus/amount.h>
#include <consensus/params.h>
#include <key.h>
#include <common/signmessage.h>
#include <primitives/transaction.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>

#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <vector>

namespace {
void initialize_sidechain()
{
    static const auto testing_setup{MakeNoLogFileContext<>()};
}
} // namespace

FUZZ_TARGET(sidechain_scripts, .init = initialize_sidechain)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    const CScript script{ConsumeScript(fdp)};
    const CTxOut out{fdp.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY), script};
    if (const auto withdrawal{sidechain::ParseWithdrawalOutput(out)}) {
        assert(withdrawal->amount >= 0 && MoneyRange(withdrawal->amount));
        const CScript rebuilt{sidechain::WithdrawalScript(withdrawal->main_fee, withdrawal->refund_keyhash, withdrawal->main_script)};
        assert(sidechain::ParseWithdrawalOutput(CTxOut{out.nValue, rebuilt}));
    }
    if (const auto refund{sidechain::ParseRefundScript(script)}) {
        const auto again{sidechain::ParseRefundScript(sidechain::RefundScript(*refund))};
        assert(again && again->withdrawal == refund->withdrawal && again->signature == refund->signature);
    }
    if (const auto commit{sidechain::ParseBundleCommitScript(script)}) {
        assert(sidechain::ParseBundleCommitScript(sidechain::BundleCommitScript(*commit)) == commit);
    }
    // Old and new forms of the record of a mainchain block read back alike.
    DataStream stream{fdp.ConsumeBytes<std::byte>(fdp.ConsumeIntegralInRange<size_t>(0, 400))};
    try {
        sidechain::MainBlock block;
        stream >> block;
        DataStream again{};
        again << block;
        sidechain::MainBlock read;
        again >> read;
        assert(read == block);
    } catch (const std::ios_base::failure&) {
    }
}

FUZZ_TARGET(sidechain_state, .init = initialize_sidechain)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    Consensus::SidechainParams params;
    params.enabled = true;
    params.min_withdrawal = fdp.ConsumeIntegralInRange<int64_t>(0, COIN);
    params.max_bundle_withdrawals = fdp.ConsumeIntegralInRange<uint32_t>(1, 4);
    params.bundle_retry_delay = fdp.ConsumeIntegralInRange<int>(0, 5);
    params.pending_min_score = fdp.ConsumeIntegralInRange<uint32_t>(0, 4);
    params.unproposed_expiry_blocks = fdp.ConsumeIntegralInRange<int>(1, 8);

    CKey key;
    const std::vector<unsigned char> secret(32, 0x42);
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    assert(key.IsValid());
    const uint160 keyhash{key.GetPubKey().GetID()};
    const CScript pay{CScript() << OP_0 << std::vector<unsigned char>(20, 7)};

    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    assert(mainchain.Append(genesis));
    // The state on a store: the chainstate's overlay, and one per block over it.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay cache{empty, /*journal=*/false};
    std::vector<COutPoint> made;
    std::vector<uint256> proposed_hashes;
    uint32_t counter{0};

    for (int height{1}; height <= 40 && fdp.remaining_bytes() > 0; ++height) {
        // The mainchain moves on: a block that may propose, close (paid or failed), or pay a bundle of another branch.
        sidechain::MainBlock main;
        main.hash = uint256{static_cast<uint8_t>(height + 1)};
        main.prev_hash = mainchain.TipHash();
        if (fdp.ConsumeBool()) {
            const uint256 hash{static_cast<uint8_t>(fdp.ConsumeIntegralInRange<int>(100, 110))};
            main.proposed.push_back(hash);
            proposed_hashes.push_back(hash);
        }
        const sidechain::State tip_state{cache};
        if (!proposed_hashes.empty() && fdp.ConsumeBool()) {
            main.bundles.push_back({proposed_hashes[fdp.ConsumeIntegralInRange<size_t>(0, proposed_hashes.size() - 1)], fdp.ConsumeBool()});
        }
        if (const auto bundle{tip_state.Bundle()}; bundle && fdp.ConsumeBool()) main.bundles.push_back({bundle->hash, fdp.ConsumeBool()});
        if (const auto bundle{tip_state.Bundle()}; bundle && fdp.ConsumeBool()) main.proposed.push_back(bundle->hash);
        // The bundles pending after the block, as the mainchain says, with scores.
        for (const uint256& hash : proposed_hashes) {
            if (fdp.ConsumeBool()) main.pending.push_back({hash, fdp.ConsumeIntegralInRange<uint32_t>(0, 6)});
        }
        if (const auto bundle{tip_state.Bundle()}; bundle && fdp.ConsumeBool()) main.pending.push_back({bundle->hash, fdp.ConsumeIntegralInRange<uint32_t>(0, 6)});
        if (fdp.ConsumeBool()) {
            sidechain::MainDeposit change;
            change.destination = drivechain::WITHDRAWAL_RETURN_DEST;
            change.bundle = uint256{static_cast<uint8_t>(fdp.ConsumeIntegralInRange<int>(200, 210))};
            change.payouts.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(0, 3 * COIN), pay);
            main.deposits.push_back(change);
        }
        assert(mainchain.Append(main));

        const uint256 before{sidechain::StoreHash(cache)};
        sidechain::StoreOverlay block{cache, /*journal=*/true};
        sidechain::State state{block};
        std::vector<CTxOut> payouts, tx_payouts;
        std::string reason;
        if (!state.ApplyMainEvents(mainchain.Height(), mainchain, height, params, payouts, reason)) continue;
        const bool main_pending{state.MainPending(mainchain, params)};
        {
            // A pending bundle is one with support, and not this chain's own; it does not hold a new
            // bundle back (the mainchain's single payout sees to it).
            const auto ours{state.Bundle()};
            const auto last{*mainchain.GetBlock(state.MainHeight())};
            const bool supported{std::any_of(last.pending.begin(), last.pending.end(), [&](const sidechain::MainPendingBundle& b) {
                return b.score >= params.pending_min_score && (!ours || b.hash != ours->hash);
            })};
            assert(main_pending == supported);
            // A bundle left pending is one the mainchain proposed in time, or that is still in time.
            if (ours && state.BundleMainHeight() >= 0 && state.MainHeight() - state.BundleMainHeight() >= params.unproposed_expiry_blocks) {
                assert(mainchain.ProposedBetween(ours->hash, state.BundleMainHeight(), state.MainHeight()));
            }
            // A bundle pending is always on record with the mainchain block that committed to it.
            if (ours) assert(state.BundleMainHeight() >= 0);
        }
        if (const auto bundle{state.NextBundle(height, uint256{static_cast<uint8_t>(height)}, params)}; bundle && fdp.ConsumeBool()) {
            assert(state.StartBundle(bundle->GetHash().ToUint256(), height, uint256{static_cast<uint8_t>(height)}, params, reason));
        }
        // Withdrawals and refunds.
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++counter)}), counter});
        const int outs{fdp.ConsumeIntegralInRange<int>(0, 3)};
        for (int i{0}; i < outs; ++i) {
            if (!made.empty() && fdp.ConsumeBool()) {
                sidechain::RefundRequest refund;
                refund.withdrawal = made[fdp.ConsumeIntegralInRange<size_t>(0, made.size() - 1)];
                assert(key.SignCompact(MessageHash(sidechain::RefundMessage(refund.withdrawal)), refund.signature));
                std::string why;
                const bool ok{state.CheckRefund(refund, why, main_pending)};
                // The peg: never a refund while a bundle of this sidechain is pending on the mainchain.
                if (main_pending) assert(!ok);
                tx.vout.emplace_back(0, sidechain::RefundScript(refund));
            } else {
                tx.vout.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(0, 2 * COIN),
                                     sidechain::WithdrawalScript(fdp.ConsumeIntegralInRange<CAmount>(0, COIN / 10), keyhash, pay));
            }
        }
        // A transaction that breaks a rule changes nothing.
        const uint256 before_tx{state.Hash()};
        if (!state.ApplyTx(CTransaction{tx}, height, params, tx_payouts, reason, main_pending)) {
            assert(state.Hash() == before_tx);
            continue;
        }
        for (uint32_t n{0}; n < tx.vout.size(); ++n) {
            if (state.GetWithdrawal(COutPoint{tx.GetHash(), n})) made.emplace_back(tx.GetHash(), n);
        }
        {
            // Never more than a block can pay; a queue with payouts owed gets at least its half, or
            // all it has.
            const size_t queued{state.Queue().size() + payouts.size()}, queued_tx{state.TxQueue().size() + tx_payouts.size()};
            const auto paid{state.TakePayouts(payouts, tx_payouts)};
            assert(paid.size() == std::min(queued + queued_tx, sidechain::MAX_PAYOUTS_PER_BLOCK));
            const size_t half{sidechain::MAX_PAYOUTS_PER_BLOCK / 2};
            assert(queued - state.Queue().size() >= std::min(queued, half));
            assert(queued_tx - state.TxQueue().size() >= std::min(queued_tx, half));
        }
        const sidechain::StoreUndo undo{block.TakeUndo()};
        block.MergeInto(cache);

        // Undone, the state is what it was, through serialization of the undo data too.
        DataStream stream{};
        stream << undo;
        sidechain::StoreUndo undo_read;
        stream >> undo_read;
        {
            sidechain::StoreOverlay reverted{cache, /*journal=*/false};
            reverted.Revert(undo_read);
            assert(sidechain::StoreHash(reverted) == before);
        }
        // The indexes agree with the withdrawals: the next bundle is the highest fees, then outpoints.
        const sidechain::State after{cache};
        std::vector<sidechain::Withdrawal> all;
        after.ForEachWithdrawal([&](const sidechain::Withdrawal& w) { all.push_back(w); return true; });
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.main_fee != b.main_fee ? a.main_fee > b.main_fee : a.outpoint < b.outpoint; });
        std::vector<COutPoint> next;
        if (!after.Bundle() && after.NextBundle(height + params.bundle_retry_delay + 1, uint256{}, params, &next)) {
            for (size_t i{0}; i < next.size(); ++i) assert(next[i] == all[i].outpoint);
        }
    }
}
