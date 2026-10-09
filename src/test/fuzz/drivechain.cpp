// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Drivechains under fuzzing: every message parser gives back what its builder makes of the result,
// and the sidechain database, fed blocks of fuzzed drivechain messages and escrow transactions,
// always comes back exactly to where it was when a block is undone, or fails part way.

#include <consensus/amount.h>
#include <consensus/params.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <cassert>
#include <optional>
#include <string>
#include <vector>

using namespace drivechain;

FUZZ_TARGET(drivechain_messages)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    const CScript script{ConsumeScript(fdp)};

    if (const auto slot{ParseEscrowScript(script)}) assert(ParseEscrowScript(EscrowScript(*slot)) == slot);
    if (const auto proposal{ParseProposalScript(script)}) {
        const auto again{ParseProposalScript(ProposalScript(*proposal))};
        assert(again && *again == *proposal);
        (void)proposal->IsValid(MAX_SLOTS);
    }
    if (const auto ack{ParseAckScript(script)}) assert(ParseAckScript(AckScript(ack->first, ack->second)) == ack);
    if (const auto bundle{ParseBundleScript(script)}) assert(ParseBundleScript(BundleScript(bundle->first, bundle->second)) == bundle);
    if (const auto votes{ParseVoteScript(script)}) {
        const CScript rebuilt{VoteScript(*votes)};
        const auto again{ParseVoteScript(rebuilt)};
        assert(again && VoteScript(*again) == rebuilt);
    }
    if (const auto accept{ParseBmmAcceptScript(script)}) assert(ParseBmmAcceptScript(BmmAcceptScript(accept->first, accept->second)) == accept);
    if (const auto request{ParseBmmRequestScript(script)}) {
        const auto again{ParseBmmRequestScript(BmmRequestScript(*request))};
        assert(again && again->slot == request->slot && again->side_block_hash == request->side_block_hash &&
               again->prev_main_block_hash == request->prev_main_block_hash);
    }
    if (const auto destination{ParseDestinationScript(script)}) assert(ParseDestinationScript(DestinationScript(*destination)) == destination);
    if (const auto fee{ParseWithdrawalFeeScript(script)}) {
        assert(MoneyRange(*fee));
        assert(ParseWithdrawalFeeScript(WithdrawalFeeScript(*fee)) == fee);
    }

    // Deposit addresses: what a valid one names formats to an address that names the same (the slot
    // may be written with leading zeros, which its checksum covers: another spelling, the same
    // deposit); a formatted one is valid.
    const std::string text{fdp.ConsumeRandomLengthString(200)};
    DepositAddress parsed;
    if (ParseDepositAddress(text, parsed) == DepositAddressKind::VALID) {
        DepositAddress again;
        assert(ParseDepositAddress(FormatDepositAddress(parsed), again) == DepositAddressKind::VALID);
        assert(again.slot == parsed.slot && again.address == parsed.address);
    }
    DepositAddress made;
    made.slot = fdp.ConsumeIntegralInRange<SidechainId>(0, MAX_SLOTS - 1);
    made.address = fdp.ConsumeRandomLengthString(MAX_DEPOSIT_DESTINATION_SIZE);
    DepositAddress back;
    const auto kind{ParseDepositAddress(FormatDepositAddress(made), back)};
    if (kind == DepositAddressKind::VALID) assert(back.slot == made.slot && back.address == made.address);
}

FUZZ_TARGET(drivechain_scdb)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    Consensus::DrivechainParams params;
    params.max_sidechains = 4;
    params.activation_period = fdp.ConsumeIntegralInRange<int>(1, 5);
    params.activation_max_failures = fdp.ConsumeIntegralInRange<int>(0, 3);
    params.replacement_period = fdp.ConsumeIntegralInRange<int>(1, 8);
    params.withdrawal_period = fdp.ConsumeIntegralInRange<int>(1, 12);
    params.withdrawal_min_score = fdp.ConsumeIntegralInRange<int>(1, 6);
    params.max_pending_bundles = fdp.ConsumeIntegralInRange<uint32_t>(1, 4);
    params.idle_expiry_blocks = fdp.ConsumeIntegralInRange<int>(1, 12);
    params.upvote_expiry_blocks = fdp.ConsumeIntegralInRange<int>(1, 8);

    SidechainDB scdb;
    uint256 tip{1};
    scdb.SetBlockHash(tip);
    uint32_t counter{0};
    std::vector<uint256> bundles;

    for (int height{1}; height <= 60 && fdp.remaining_bytes() > 0; ++height) {
        CBlock block;
        block.hashPrevBlock = tip;
        block.nTime = ++counter;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].scriptSig = CScript() << height << OP_0;
        coinbase.vout.emplace_back(50 * COIN, CScript() << OP_TRUE);
        std::vector<CMutableTransaction> txs;
        const int messages{fdp.ConsumeIntegralInRange<int>(0, 4)};
        for (int m{0}; m < messages; ++m) {
            const SidechainId slot{fdp.ConsumeIntegralInRange<SidechainId>(0, params.max_sidechains)};
            switch (fdp.ConsumeIntegralInRange<int>(0, 6)) {
            case 0: {
                Sidechain sidechain;
                sidechain.slot = slot;
                sidechain.title = fdp.ConsumeBool() ? "a" : "b";
                coinbase.vout.emplace_back(0, ProposalScript(sidechain));
                coinbase.vout.emplace_back(0, AckScript(slot, sidechain.GetHash()));
                break;
            }
            case 1: {
                Sidechain sidechain;
                sidechain.slot = slot;
                sidechain.title = fdp.ConsumeBool() ? "a" : "b";
                coinbase.vout.emplace_back(0, AckScript(slot, sidechain.GetHash()));
                break;
            }
            case 2: {
                const uint256 hash{static_cast<uint8_t>(fdp.ConsumeIntegralInRange<int>(1, 8))};
                bundles.push_back(hash);
                coinbase.vout.emplace_back(0, BundleScript(slot, hash));
                break;
            }
            case 3: {
                std::vector<uint16_t> votes;
                const size_t count{fdp.ConsumeIntegralInRange<size_t>(0, 4)};
                for (size_t i{0}; i < count; ++i) votes.push_back(fdp.PickValueInArray<uint16_t>({0, 1, 2, VOTE_ABSTAIN, VOTE_DOWNVOTE}));
                coinbase.vout.emplace_back(0, VoteScript(MakeVoteMessage(votes)));
                break;
            }
            case 4: {
                // A deposit to the escrow of the slot, as it stands.
                CMutableTransaction tx;
                tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++counter)}), counter});
                CAmount have{0};
                if (const Slot* state{scdb.GetSlot(slot)}; state && state->has_ctip) {
                    tx.vin.emplace_back(state->ctip.outpoint);
                    have = state->ctip.amount;
                }
                tx.vout.emplace_back(have + fdp.ConsumeIntegralInRange<CAmount>(0, 10 * COIN), EscrowScript(slot));
                tx.vout.emplace_back(0, DestinationScript(fdp.ConsumeBool() ? "dest" : WITHDRAWAL_RETURN_DEST));
                txs.push_back(tx);
                break;
            }
            case 5: {
                // A withdrawal of a pending bundle of the slot, paying a fuzzed amount out of the escrow.
                const Slot* state{scdb.GetSlot(slot)};
                if (!state || !state->has_ctip || state->bundles.empty()) break;
                CMutableTransaction tx;
                tx.vin.emplace_back(state->ctip.outpoint);
                const CAmount payout{fdp.ConsumeIntegralInRange<CAmount>(0, state->ctip.amount)};
                const CAmount fee{fdp.ConsumeIntegralInRange<CAmount>(0, state->ctip.amount - payout)};
                tx.vout.emplace_back(state->ctip.amount - payout - fee, EscrowScript(slot));
                tx.vout.emplace_back(payout, CScript() << OP_TRUE << static_cast<int64_t>(counter++));
                txs.push_back(tx);
                break;
            }
            case 6:
                coinbase.vout.emplace_back(0, BmmAcceptScript(slot, uint256{static_cast<uint8_t>(m + 1)}));
                break;
            }
        }
        block.vtx.push_back(MakeTransactionRef(coinbase));
        for (const auto& tx : txs) block.vtx.push_back(MakeTransactionRef(tx));

        const SidechainDB before{scdb};
        SidechainDB after{scdb};
        BlockUndo undo;
        std::vector<Deposit> deposits;
        std::string reason;
        if (!after.ConnectBlock(block, height, params, undo, &deposits, reason)) {
            assert(!reason.empty());
            // A block that fails is taken back by what its undo data has so far.
            after.DisconnectBlock(undo);
            assert(after == before);
            assert(after.GetHash() == before.GetHash());
            continue;
        }
        // Undo brings back exactly what was, through serialization too.
        DataStream stream{};
        stream << undo;
        BlockUndo undo_read;
        stream >> undo_read;
        SidechainDB reverted{after};
        reverted.DisconnectBlock(undo_read);
        assert(reverted == before);
        assert(reverted.GetHash() == before.GetHash());
        DataStream state_stream{};
        state_stream << after;
        SidechainDB state_read;
        state_stream >> state_read;
        assert(state_read == after);
        // What the database promises at every block.
        for (const auto& [id, slot] : after.GetSlots()) {
            assert(slot.bundles.size() <= params.max_pending_bundles);
            if (slot.has_ctip) assert(MoneyRange(slot.ctip.amount));
            for (const Bundle& bundle : slot.bundles) assert(!after.IsClosed(id, bundle.hash));
        }
        scdb = after;
        tip = block.GetHash();
    }
}
