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
    params.single_bundle_height = fdp.ConsumeIntegralInRange<int>(0, 30);

    CKey key;
    key.Set(std::vector<unsigned char>(32, 0x42).begin(), std::vector<unsigned char>(32, 0x42).end(), /*fCompressedIn=*/true);
    const uint160 keyhash{key.GetPubKey().GetID()};
    const CScript pay{CScript() << OP_0 << std::vector<unsigned char>(20, 7)};

    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    assert(mainchain.Append(genesis));
    sidechain::State state;
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
        if (!proposed_hashes.empty() && fdp.ConsumeBool()) {
            main.bundles.push_back({proposed_hashes[fdp.ConsumeIntegralInRange<size_t>(0, proposed_hashes.size() - 1)], fdp.ConsumeBool()});
        }
        if (state.Bundle() && fdp.ConsumeBool()) main.bundles.push_back({state.Bundle()->hash, fdp.ConsumeBool()});
        if (fdp.ConsumeBool()) {
            sidechain::MainDeposit change;
            change.destination = drivechain::WITHDRAWAL_RETURN_DEST;
            change.bundle = uint256{static_cast<uint8_t>(fdp.ConsumeIntegralInRange<int>(200, 210))};
            change.payouts.emplace_back(fdp.ConsumeIntegralInRange<CAmount>(0, 3 * COIN), pay);
            main.deposits.push_back(change);
        }
        assert(mainchain.Append(main));

        const sidechain::State before{state};
        sidechain::StateUndo undo;
        undo.main_height = state.MainHeight();
        undo.bundle = state.Bundle();
        undo.last_failure_height = state.LastFailureHeight();
        std::vector<CTxOut> payouts, tx_payouts;
        std::string reason;
        if (!state.ApplyMainEvents(mainchain.Height(), mainchain, height, params, undo, payouts, reason)) {
            state = before;
            continue;
        }
        const bool main_pending{state.MainPending(mainchain, height, params)};
        if (main_pending) assert(!state.NextBundle(height, uint256{static_cast<uint8_t>(height)}, params, nullptr, main_pending));
        if (const auto bundle{state.NextBundle(height, uint256{static_cast<uint8_t>(height)}, params, nullptr, main_pending)}; bundle && fdp.ConsumeBool()) {
            assert(state.StartBundle(bundle->GetHash().ToUint256(), height, uint256{static_cast<uint8_t>(height)}, params, undo, reason, main_pending));
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
        if (!state.ApplyTx(CTransaction{tx}, height, params, undo, tx_payouts, reason, main_pending)) {
            state = before;
            continue;
        }
        for (uint32_t n{0}; n < tx.vout.size(); ++n) {
            if (state.Withdrawals().contains(COutPoint{tx.GetHash(), n})) made.emplace_back(tx.GetHash(), n);
        }
        (void)state.TakePayouts(payouts, tx_payouts, undo);

        // Undone, the state is what it was, through serialization of the undo data too.
        DataStream stream{};
        stream << undo;
        sidechain::StateUndo undo_read;
        stream >> undo_read;
        sidechain::State reverted{state};
        reverted.DisconnectBlock(undo_read);
        assert(reverted == before);
        DataStream state_stream{};
        state_stream << state;
        sidechain::State state_read;
        state_stream >> state_read;
        assert(state_read == state);
    }
}
