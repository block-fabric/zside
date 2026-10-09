// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <hash.h>
#include <util/strencodings.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <core_io.h>
#include <dbwrapper.h>
#include <drivechain/db.h>
#include <drivechain/miner.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <common/signmessage.h>
#include <key.h>
#include <key_io.h>
#include <hash.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <streams.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>
#include <crypto/common.h>
#include <tinyformat.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace drivechain;

namespace {

Consensus::DrivechainParams TestParams()
{
    Consensus::DrivechainParams params;
    params.max_sidechains = 8;
    params.activation_period = 10;
    params.activation_max_failures = 4;
    params.replacement_period = 20;
    params.withdrawal_period = 30;
    params.withdrawal_min_score = 10;
    params.max_pending_bundles = 3;
    return params;
}

/** A vote message with these votes, one per active sidechain. */
CScript Votes(const std::vector<uint16_t>& votes) { return VoteScript(MakeVoteMessage(votes)); }

CScript Ack(const Sidechain& sidechain) { return AckScript(sidechain.slot, sidechain.GetHash()); }

Sidechain MakeSidechain(SidechainId slot, const std::string& title = "test")
{
    Sidechain sidechain;
    sidechain.slot = slot;
    sidechain.title = title;
    sidechain.description = "a sidechain";
    sidechain.hash_id1 = uint256{1};
    return sidechain;
}

/** A chain of blocks with the sidechain database that follows from it. */
struct TestChain {
    Consensus::DrivechainParams params{TestParams()};
    SidechainDB scdb;
    int height{0};
    uint256 tip{uint256{0xaa}};
    uint32_t counter{0};
    std::string reject_reason;
    std::vector<Deposit> deposits;

    TestChain() { scdb.SetBlockHash(tip); }

    CBlock MakeBlock(const std::vector<CScript>& coinbase_scripts, const std::vector<CMutableTransaction>& txs = {})
    {
        CBlock block;
        block.hashPrevBlock = tip;
        block.nTime = ++counter;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        coinbase.vout.emplace_back(50 * COIN, CScript() << OP_TRUE);
        for (const CScript& script : coinbase_scripts) coinbase.vout.emplace_back(0, script);
        block.vtx.push_back(MakeTransactionRef(coinbase));
        for (const CMutableTransaction& tx : txs) block.vtx.push_back(MakeTransactionRef(tx));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        return block;
    }

    /**
     * Connect a block. Whenever a block is valid, also check that disconnecting
     * it restores the previous state exactly, and that the undo data survives
     * serialization.
     */
    bool Connect(const std::vector<CScript>& coinbase_scripts = {}, const std::vector<CMutableTransaction>& txs = {})
    {
        const CBlock block{MakeBlock(coinbase_scripts, txs)};
        const SidechainDB before{scdb};
        SidechainDB after{scdb};
        BlockUndo undo;
        deposits.clear();
        reject_reason.clear();
        if (!after.ConnectBlock(block, height + 1, params, undo, &deposits, reject_reason)) {
            BOOST_CHECK(!reject_reason.empty());
            // Failed part way, the block is taken back by its undo data so far (as validation does).
            after.DisconnectBlock(undo);
            BOOST_CHECK(after == before);
            BOOST_CHECK(after.GetHash() == before.GetHash());
            return false;
        }
        BOOST_CHECK(after.GetBlockHash() == block.GetHash());

        DataStream stream{};
        stream << undo;
        BlockUndo undo_read;
        stream >> undo_read;
        SidechainDB reverted{after};
        reverted.DisconnectBlock(undo_read);
        BOOST_CHECK(reverted == before);
        BOOST_CHECK(reverted.GetHash() == before.GetHash());

        DataStream state_stream{};
        state_stream << after;
        SidechainDB state_read;
        state_stream >> state_read;
        BOOST_CHECK(state_read == after);

        scdb = after;
        tip = block.GetHash();
        ++height;
        return true;
    }

    void ConnectEmpty(int count)
    {
        for (int i{0}; i < count; ++i) BOOST_REQUIRE(Connect());
    }

    /** Propose a sidechain and ack it until it activates. */
    void Activate(const Sidechain& sidechain)
    {
        const bool replacing{scdb.IsActive(sidechain.slot)};
        const int period{replacing ? params.replacement_period : params.activation_period};
        BOOST_REQUIRE(Connect({ProposalScript(sidechain)}));
        for (int i{1}; i < period; ++i) BOOST_REQUIRE(Connect({Ack(sidechain)}));
        BOOST_REQUIRE(scdb.IsActive(sidechain.slot));
        BOOST_REQUIRE(scdb.GetSlot(sidechain.slot)->sidechain == sidechain);
    }

    /** A transaction spending some unrelated coin. */
    CMutableTransaction BaseTx()
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++counter)}), counter};
        return tx;
    }

    /** A deposit that brings the escrow of `slot` to `total`. */
    CMutableTransaction DepositTx(SidechainId slot, CAmount total, const std::string& destination = "dest")
    {
        CMutableTransaction tx{BaseTx()};
        if (const Slot* state{scdb.GetSlot(slot)}; state && state->has_ctip) tx.vin.emplace_back(state->ctip.outpoint);
        tx.vout.emplace_back(total, EscrowScript(slot));
        tx.vout.emplace_back(0, DestinationScript(destination));
        return tx;
    }

    /** A withdrawal bundle paying `payout` with `fee`, in the blind form the sidechain produces. */
    static CMutableTransaction BlindBundle(CAmount payout, CAmount fee, uint8_t salt = 0)
    {
        CMutableTransaction tx;
        tx.vout.emplace_back(0, WithdrawalFeeScript(fee));
        tx.vout.emplace_back(payout, CScript() << OP_TRUE << salt);
        return tx;
    }

    /** Complete a blind bundle into the transaction that pays it out of the escrow. */
    CMutableTransaction WithdrawalTx(SidechainId slot, const CMutableTransaction& blind, CAmount payout, CAmount fee)
    {
        const Slot* state{scdb.GetSlot(slot)};
        BOOST_REQUIRE(state && state->has_ctip);
        CMutableTransaction tx{blind};
        tx.vin.assign(1, CTxIn{state->ctip.outpoint});
        tx.vout[0] = CTxOut{state->ctip.amount - payout - fee, EscrowScript(slot)};
        return tx;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(drivechain_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(scripts)
{
    for (const SidechainId slot : {0U, 1U, 2U, 16U, 255U}) {
        BOOST_CHECK(ParseEscrowScript(EscrowScript(slot)) == slot);
    }
    BOOST_CHECK(!ParseEscrowScript(CScript() << OP_DRIVECHAIN));
    BOOST_CHECK(!ParseEscrowScript(CScript() << OP_TRUE));

    const Sidechain sidechain{MakeSidechain(5)};
    BOOST_CHECK(ParseProposalScript(ProposalScript(sidechain)) == sidechain);
    BOOST_CHECK((ParseAckScript(Ack(sidechain)) == std::make_pair(SidechainId{5}, sidechain.GetHash())));
    BOOST_CHECK(!ParseAckScript(ProposalScript(sidechain)));
    BOOST_CHECK(!ParseProposalScript(Ack(sidechain)));

    const uint256 hash{7};
    BOOST_CHECK((ParseBundleScript(BundleScript(200, hash)) == std::make_pair(SidechainId{200}, hash)));
    BOOST_CHECK((ParseBmmAcceptScript(BmmAcceptScript(3, hash)) == std::make_pair(SidechainId{3}, hash)));
    BOOST_CHECK(!ParseBmmAcceptScript(BundleScript(3, hash)));

    const std::vector<uint16_t> votes{0, VOTE_ABSTAIN, VOTE_DOWNVOTE, 2};
    BOOST_CHECK(ParseVoteScript(Votes(votes))->votes == votes);
    BOOST_CHECK(ParseVoteScript(Votes(votes))->form == VoteForm::ONE_BYTE);
    const std::vector<uint16_t> wide{300, VOTE_ABSTAIN};
    BOOST_CHECK(ParseVoteScript(Votes(wide))->votes == wide);
    BOOST_CHECK(ParseVoteScript(Votes(wide))->form == VoteForm::TWO_BYTES);
    BOOST_CHECK(ParseVoteScript(VoteScript({VoteForm::REPEAT_PREVIOUS, {}}))->form == VoteForm::REPEAT_PREVIOUS);
    BOOST_CHECK(ParseVoteScript(VoteScript({VoteForm::LEADING_BY_50, {}}))->form == VoteForm::LEADING_BY_50);

    BmmRequest request;
    request.slot = 2;
    request.side_block_hash = uint256{3};
    request.prev_main_block_hash = uint256{4};
    const auto parsed{ParseBmmRequestScript(BmmRequestScript(request))};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed->slot == 2 && parsed->side_block_hash == uint256{3} && parsed->prev_main_block_hash == uint256{4});

    BOOST_CHECK(ParseDestinationScript(DestinationScript("hello")) == "hello");
    BOOST_CHECK(ParseWithdrawalFeeScript(WithdrawalFeeScript(1234)) == 1234);
    BOOST_CHECK(!ParseWithdrawalFeeScript(DestinationScript("hello")));

    // Everything but the escrow script is unspendable.
    BOOST_CHECK(ProposalScript(sidechain).IsUnspendable());
    BOOST_CHECK(!EscrowScript(1).IsUnspendable());

    // Anyone can spend an escrow output, with an empty scriptSig, under the standard rules too
    // (minimal pushes, clean stack): the drivechain rules decide.
    for (const SidechainId slot : {0U, 2U, 9U, 16U, 255U}) {
        ScriptError error;
        BOOST_CHECK_MESSAGE(VerifyScript(CScript(), EscrowScript(slot), nullptr, STANDARD_SCRIPT_VERIFY_FLAGS, BaseSignatureChecker{}, &error), ScriptErrorString(error));
    }
}

BOOST_AUTO_TEST_CASE(enforcer_byte_layouts)
{
    // The exact bytes of the BIP300/301 messages, as the Layer Two Labs enforcer writes and reads them.
    BOOST_CHECK_EQUAL(HexStr(EscrowScript(9)), "b4010951");
    BOOST_CHECK_EQUAL(HexStr(EscrowScript(0)), "b4010051");
    uint256 hash;
    for (size_t i{0}; i < 32; ++i) hash.begin()[i] = i;
    const std::string hash_hex{HexStr(hash)};
    BOOST_CHECK_EQUAL(HexStr(AckScript(2, hash)), "6a25" "d6e1c5df" "02" + hash_hex);
    BOOST_CHECK_EQUAL(HexStr(BundleScript(2, hash)), "6a25" "d45aa943" "02" + hash_hex);
    BOOST_CHECK_EQUAL(HexStr(BmmAcceptScript(9, hash)), "6a25" "d1617368" "09" + hash_hex);
    BOOST_CHECK_EQUAL(HexStr(Votes({0, VOTE_ABSTAIN, VOTE_DOWNVOTE})), "6a08" "d77d1776" "01" "00fffe");
    BOOST_CHECK_EQUAL(HexStr(Votes({256, VOTE_ABSTAIN})), "6a09" "d77d1776" "02" "0001ffff");
    BOOST_CHECK_EQUAL(HexStr(VoteScript({VoteForm::REPEAT_PREVIOUS, {}})), "6a05" "d77d1776" "00");
    BOOST_CHECK_EQUAL(HexStr(VoteScript({VoteForm::LEADING_BY_50, {}})), "6a05" "d77d1776" "03");
    BmmRequest request;
    request.slot = 2;
    request.side_block_hash = hash;
    request.prev_main_block_hash = hash;
    BOOST_CHECK_EQUAL(HexStr(BmmRequestScript(request)), "6a44" "00bf00" "02" + hash_hex + hash_hex);
    BOOST_CHECK_EQUAL(HexStr(WithdrawalFeeScript(0x0102)), "6a08" "0000000000000102");

    // M1: slot, then the description: version 0, title length, title, description, hash_id1, hash_id2.
    Sidechain sidechain{MakeSidechain(9, "ab")};
    sidechain.description = "c";
    sidechain.hash_id1 = uint256{};
    BOOST_CHECK_EQUAL(HexStr(ProposalScript(sidechain)), "6a3e" "d5e0c4af" "09" "00" "02" "6162" "63" + std::string(104, '0'));
    BOOST_CHECK(sidechain.GetHash() == Hash(sidechain.Description()));

    // An M8 counts as output 0 only.
    CMutableTransaction tx;
    tx.vout.emplace_back(0, BmmRequestScript(request));
    BOOST_CHECK(GetBmmRequest(CTransaction{tx}));
    tx.vout.insert(tx.vout.begin(), CTxOut{1000, CScript() << OP_TRUE});
    BOOST_CHECK(!GetBmmRequest(CTransaction{tx}));
}

BOOST_AUTO_TEST_CASE(proposal_limits)
{
    Sidechain sidechain{MakeSidechain(0)};
    BOOST_CHECK(sidechain.IsValid(8));
    BOOST_CHECK(!MakeSidechain(8).IsValid(8));
    sidechain.title.clear();
    BOOST_CHECK(!sidechain.IsValid(8));
    sidechain.title = std::string(MAX_TITLE_SIZE + 1, 'a');
    BOOST_CHECK(!sidechain.IsValid(8));
    sidechain.title = "ok";
    sidechain.description = std::string(MAX_DESCRIPTION_SIZE + 1, 'a');
    BOOST_CHECK(!sidechain.IsValid(8));
    sidechain.description.clear();
    sidechain.version = SIDECHAIN_VERSION_MAX + 1;
    BOOST_CHECK(!sidechain.IsValid(8));
}

BOOST_AUTO_TEST_CASE(activation)
{
    TestChain chain;
    const Sidechain sidechain{MakeSidechain(3)};
    const uint256 hash{sidechain.GetHash()};

    // An ack for something that was never proposed is ignored.
    BOOST_REQUIRE(chain.Connect({Ack(sidechain)}));
    BOOST_CHECK(chain.scdb.GetProposals().empty());

    BOOST_REQUIRE(chain.Connect({ProposalScript(sidechain)}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetProposals().size(), 1U);
    BOOST_CHECK(!chain.scdb.IsActive(3));
    BOOST_CHECK(chain.scdb.GetProposal(3, hash));
    BOOST_CHECK(!chain.scdb.GetProposal(4, hash));

    // Proposing it again does not reset it; a block cannot hold the same proposal twice.
    BOOST_REQUIRE(chain.Connect({ProposalScript(sidechain), Ack(sidechain)}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetProposals().size(), 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetProposals()[0].acks, 1U);
    BOOST_CHECK(!chain.Connect({ProposalScript(MakeSidechain(4)), ProposalScript(MakeSidechain(4))}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-duplicate-proposal");

    // With an ack in every block it activates when its age reaches the period.
    for (int i{2}; i < chain.params.activation_period - 1; ++i) {
        BOOST_REQUIRE(chain.Connect({Ack(sidechain)}));
        BOOST_CHECK(!chain.scdb.IsActive(3));
    }
    BOOST_REQUIRE(chain.Connect({Ack(sidechain)}));
    BOOST_CHECK(chain.scdb.IsActive(3));
    BOOST_CHECK(chain.scdb.GetProposals().empty());
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(3)->activation_height, chain.height);

    // Proposals for slots this version does not have are ignored.
    BOOST_REQUIRE(chain.Connect({ProposalScript(MakeSidechain(8))}));
    BOOST_CHECK(chain.scdb.GetProposals().empty());

    // A block can make several proposals.
    BOOST_REQUIRE(chain.Connect({ProposalScript(MakeSidechain(4)), ProposalScript(MakeSidechain(5))}));
    BOOST_CHECK_EQUAL(chain.scdb.GetProposals().size(), 2U);
}

BOOST_AUTO_TEST_CASE(activation_failures)
{
    TestChain chain;
    const Sidechain sidechain{MakeSidechain(1)};
    BOOST_REQUIRE(chain.Connect({ProposalScript(sidechain)}));

    // It survives as long as the blocks without an ack stay below the limit.
    chain.ConnectEmpty(chain.params.activation_max_failures - 1);
    BOOST_REQUIRE_EQUAL(chain.scdb.GetProposals().size(), 1U);
    BOOST_CHECK_EQUAL(SidechainDB::Failures(chain.scdb.GetProposals()[0], chain.height), chain.params.activation_max_failures - 1);
    BOOST_REQUIRE(chain.Connect({Ack(sidechain)}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetProposals().size(), 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetProposals()[0].acks, 1U);

    // One more block without an ack kills it.
    chain.ConnectEmpty(1);
    BOOST_CHECK(chain.scdb.GetProposals().empty());
    BOOST_CHECK(!chain.scdb.IsActive(1));

    // It can then be proposed again.
    BOOST_CHECK(chain.Connect({ProposalScript(sidechain)}));
}

BOOST_AUTO_TEST_CASE(acks)
{
    TestChain chain;
    const Sidechain a{MakeSidechain(1, "a")};
    const Sidechain b{MakeSidechain(1, "b")};
    const Sidechain c{MakeSidechain(2, "c")};
    BOOST_REQUIRE(chain.Connect({ProposalScript(a)}));
    BOOST_REQUIRE(chain.Connect({ProposalScript(b)}));
    BOOST_REQUIRE(chain.Connect({ProposalScript(c)}));

    // One ack per slot per block.
    BOOST_CHECK(!chain.Connect({Ack(a), Ack(b)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-acks");
    BOOST_CHECK(!chain.Connect({Ack(a), Ack(a)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-acks");
    // An ack names the slot too.
    BOOST_REQUIRE(chain.Connect({AckScript(2, a.GetHash())}));
    BOOST_CHECK_EQUAL(chain.scdb.GetProposal(a.GetHash())->acks, 0U);

    // Proposals for different slots can be acked together.
    BOOST_REQUIRE(chain.Connect({Ack(a), Ack(c)}));
    BOOST_CHECK_EQUAL(chain.scdb.GetProposal(a.GetHash())->acks, 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetProposal(b.GetHash())->acks, 0U);
    BOOST_CHECK_EQUAL(chain.scdb.GetProposal(c.GetHash())->acks, 1U);

    // Acking a proposal in the block that makes it changes nothing.
    const Sidechain d{MakeSidechain(4, "d")};
    BOOST_REQUIRE(chain.Connect({ProposalScript(d), Ack(d)}));
    BOOST_CHECK_EQUAL(chain.scdb.GetProposal(d.GetHash())->acks, 0U);
}

BOOST_AUTO_TEST_CASE(deposits)
{
    TestChain chain;
    chain.Activate(MakeSidechain(2));

    // A deposit to a sidechain that does not exist is invalid.
    BOOST_CHECK(!chain.Connect({}, {chain.DepositTx(3, COIN)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-inactive-sidechain");
    // Outputs for slots this version does not have are not looked at.
    BOOST_CHECK(chain.Connect({}, {chain.DepositTx(8, COIN)}));
    BOOST_CHECK(chain.deposits.empty());

    // The first deposit creates the escrow output.
    const CMutableTransaction first{chain.DepositTx(2, 5 * COIN, "alice")};
    BOOST_REQUIRE(chain.Connect({}, {first}));
    const Slot* slot{chain.scdb.GetSlot(2)};
    BOOST_REQUIRE(slot->has_ctip);
    BOOST_CHECK((slot->ctip.outpoint == COutPoint{first.GetHash(), 0}));
    BOOST_CHECK_EQUAL(slot->ctip.amount, 5 * COIN);
    BOOST_REQUIRE_EQUAL(chain.deposits.size(), 1U);
    BOOST_CHECK_EQUAL(chain.deposits[0].destination, "alice");
    BOOST_CHECK_EQUAL(chain.deposits[0].amount, 5 * COIN);
    BOOST_CHECK_EQUAL(chain.deposits[0].total, 5 * COIN);
    BOOST_CHECK_EQUAL(chain.deposits[0].tx_index, 1U);

    // Later deposits have to spend it.
    CMutableTransaction unspent{chain.BaseTx()};
    unspent.vout.emplace_back(7 * COIN, EscrowScript(2));
    unspent.vout.emplace_back(0, DestinationScript("bob"));
    BOOST_CHECK(!chain.Connect({}, {unspent}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-escrow-unspent");

    // Two deposits in a block chain on each other.
    const CMutableTransaction second{chain.DepositTx(2, 7 * COIN, "bob")};
    CMutableTransaction third{chain.BaseTx()};
    third.vin.emplace_back(COutPoint{second.GetHash(), 0});
    third.vout.emplace_back(8 * COIN, EscrowScript(2));
    third.vout.emplace_back(0, DestinationScript("carol"));
    // Both spending the same escrow output: the second one no longer spends the current one.
    BOOST_CHECK(!chain.Connect({}, {second, chain.DepositTx(2, 9 * COIN)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-escrow-unspent");
    BOOST_REQUIRE(chain.Connect({}, {second, third}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(2)->ctip.amount, 8 * COIN);
    BOOST_REQUIRE_EQUAL(chain.deposits.size(), 2U);
    BOOST_CHECK_EQUAL(chain.deposits[0].amount, 2 * COIN);
    BOOST_CHECK_EQUAL(chain.deposits[1].amount, 1 * COIN);
    BOOST_CHECK_EQUAL(chain.deposits[1].destination, "carol");

    // A deposit needs a destination, right after the escrow output, and not the one reserved for withdrawals.
    CMutableTransaction no_dest{chain.DepositTx(2, 9 * COIN)};
    no_dest.vout.pop_back();
    BOOST_CHECK(!chain.Connect({}, {no_dest}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-deposit-destination");
    CMutableTransaction late_dest{chain.DepositTx(2, 9 * COIN)};
    late_dest.vout.insert(late_dest.vout.begin() + 1, CTxOut{COIN, CScript() << OP_TRUE});
    BOOST_CHECK(!chain.Connect({}, {late_dest}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-deposit-destination");
    BOOST_CHECK(!chain.Connect({}, {chain.DepositTx(2, 9 * COIN, WITHDRAWAL_RETURN_DEST)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-deposit-destination");
    BOOST_CHECK(!chain.Connect({}, {chain.DepositTx(2, 9 * COIN, std::string(MAX_DEPOSIT_DESTINATION_SIZE + 1, 'x'))}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-deposit-destination");

    // The escrow cannot be respent for the same amount, taken without a return output, or split.
    BOOST_CHECK(!chain.Connect({}, {chain.DepositTx(2, 8 * COIN)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-escrow-amount");
    CMutableTransaction theft{chain.BaseTx()};
    theft.vin.emplace_back(chain.scdb.GetSlot(2)->ctip.outpoint);
    theft.vout.emplace_back(8 * COIN, CScript() << OP_TRUE);
    BOOST_CHECK(!chain.Connect({}, {theft}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-escrow-spend");
    CMutableTransaction split{chain.DepositTx(2, 9 * COIN)};
    split.vout.emplace_back(COIN, EscrowScript(2));
    BOOST_CHECK(!chain.Connect({}, {split}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-escrow-outputs");

    // A coinbase cannot pay into an escrow.
    BOOST_CHECK(!chain.Connect({EscrowScript(2)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-coinbase-escrow");

    // Taking coins out without an approved bundle is invalid.
    CMutableTransaction unapproved{chain.BaseTx()};
    unapproved.vin[0].prevout = chain.scdb.GetSlot(2)->ctip.outpoint;
    unapproved.vout.emplace_back(7 * COIN, EscrowScript(2));
    unapproved.vout.emplace_back(COIN, CScript() << OP_TRUE);
    BOOST_CHECK(!chain.Connect({}, {unapproved}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-unknown");

    // Spending the escrow of one sidechain into another is invalid.
    chain.Activate(MakeSidechain(5));
    CMutableTransaction cross{chain.BaseTx()};
    cross.vin.emplace_back(chain.scdb.GetSlot(2)->ctip.outpoint);
    cross.vout.emplace_back(9 * COIN, EscrowScript(5));
    cross.vout.emplace_back(0, DestinationScript("mallory"));
    BOOST_CHECK(!chain.Connect({}, {cross}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-escrow-mismatch");
}

BOOST_AUTO_TEST_CASE(withdrawals)
{
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(1, 10 * COIN)}));

    const CAmount payout{3 * COIN};
    const CAmount fee{COIN / 10};
    CMutableTransaction blind{TestChain::BlindBundle(payout, fee)};
    // The lock time is part of what miners vote on.
    blind.nLockTime = 7;
    BOOST_CHECK(blind.GetHash() != TestChain::BlindBundle(payout, fee).GetHash());
    BOOST_CHECK(IsBlindWithdrawal(CTransaction{blind}));
    const uint256 hash{blind.GetHash().ToUint256()};
    BOOST_CHECK(BlindWithdrawalHash(CTransaction{chain.WithdrawalTx(1, blind, payout, fee)}, 10 * COIN) == hash);
    // The blind form travels as hex without the witness flag, which a transaction without inputs cannot have.
    CMutableTransaction decoded;
    BOOST_CHECK(DecodeHexTx(decoded, EncodeHexTx(CTransaction{blind}), /*try_no_witness=*/true, /*try_witness=*/false));
    BOOST_CHECK(decoded.GetHash() == blind.GetHash());

    // Bundles for sidechains that do not exist are ignored.
    BOOST_REQUIRE(chain.Connect({BundleScript(4, hash)}));
    BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.empty());

    BOOST_REQUIRE(chain.Connect({BundleScript(1, hash)}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].score, 1U);
    BOOST_CHECK(!chain.Connect({BundleScript(1, hash)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bundle-known");

    // It cannot be paid out before it has the score.
    BOOST_CHECK(!chain.Connect({}, {chain.WithdrawalTx(1, blind, payout, fee)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-score");

    // Votes: an index upvotes, abstaining and no vote message leave the score, a downvote lowers it.
    BOOST_REQUIRE(chain.Connect({Votes({0})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].score, 2U);
    BOOST_REQUIRE(chain.Connect({Votes({VOTE_ABSTAIN})}));
    BOOST_REQUIRE(chain.Connect());
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].score, 2U);
    BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].score, 1U);
    BOOST_CHECK(!chain.Connect({Votes({1})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-votes-index");
    // One vote per active sidechain, no more and no less.
    BOOST_CHECK(!chain.Connect({Votes({0, 0})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-votes-size");
    BOOST_CHECK(!chain.Connect({Votes({})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-votes-size");
    BOOST_CHECK(!chain.Connect({Votes({0}), Votes({0})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-votes");
    // The two byte form is for votes that need it.
    BOOST_CHECK(!chain.Connect({VoteScript({VoteForm::TWO_BYTES, {0}})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-votes-form");
    BOOST_REQUIRE(chain.Connect({VoteScript({VoteForm::TWO_BYTES, {VOTE_ABSTAIN}})}));

    while (chain.scdb.GetSlot(1)->bundles[0].score < static_cast<uint32_t>(chain.params.withdrawal_min_score)) {
        BOOST_REQUIRE(chain.Connect({Votes({0})}));
    }

    // The payout has to match the bundle, including the fee left to miners.
    CMutableTransaction wrong_change{chain.WithdrawalTx(1, blind, payout, fee)};
    wrong_change.vout[0].nValue += 1;
    BOOST_CHECK(!chain.Connect({}, {wrong_change}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-unknown");
    CMutableTransaction moved{chain.WithdrawalTx(1, blind, payout, fee)};
    std::swap(moved.vout[0], moved.vout[1]);
    BOOST_CHECK(!chain.Connect({}, {moved}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-outputs");
    CMutableTransaction altered{chain.WithdrawalTx(1, blind, payout, fee)};
    altered.vout[1].scriptPubKey = CScript() << OP_TRUE << OP_TRUE;
    BOOST_CHECK(!chain.Connect({}, {altered}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-unknown");
    CMutableTransaction extra_input{chain.WithdrawalTx(1, blind, payout, fee)};
    extra_input.vin.push_back(chain.BaseTx().vin[0]);
    BOOST_CHECK(!chain.Connect({}, {extra_input}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-inputs");

    const CMutableTransaction withdrawal{chain.WithdrawalTx(1, blind, payout, fee)};
    BOOST_REQUIRE(chain.Connect({}, {withdrawal}));
    const Slot* slot{chain.scdb.GetSlot(1)};
    BOOST_CHECK(slot->bundles.empty());
    BOOST_CHECK_EQUAL(slot->ctip.amount, 10 * COIN - payout - fee);
    BOOST_CHECK((slot->ctip.outpoint == COutPoint{withdrawal.GetHash(), 0}));
    BOOST_CHECK(chain.scdb.IsClosed(1, hash));
    BOOST_CHECK(chain.scdb.WasPaid(1, hash) == std::optional<bool>{true});
    BOOST_CHECK(!chain.scdb.WasPaid(0, hash));
    BOOST_REQUIRE_EQUAL(chain.deposits.size(), 1U);
    BOOST_CHECK_EQUAL(chain.deposits[0].destination, WITHDRAWAL_RETURN_DEST);
    BOOST_CHECK_EQUAL(chain.deposits[0].amount, 0);
    BOOST_CHECK_EQUAL(chain.deposits[0].total, 10 * COIN - payout - fee);

    // A bundle that was paid out cannot be proposed again.
    BOOST_CHECK(!chain.Connect({BundleScript(1, hash)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bundle-closed");
}

BOOST_AUTO_TEST_CASE(withdrawal_votes)
{
    TestChain chain;
    chain.Activate(MakeSidechain(0));
    chain.Activate(MakeSidechain(6));

    const uint256 a{0xa1}, b{0xb1}, c{0xc1}, d{0xd1};
    BOOST_REQUIRE(chain.Connect({BundleScript(0, a)}));
    BOOST_REQUIRE(chain.Connect({BundleScript(0, b), BundleScript(6, c)}));
    // One new bundle per sidechain per block.
    BOOST_CHECK(!chain.Connect({BundleScript(0, d), BundleScript(0, uint256{0xd2})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-bundles");

    // Vote entries follow slot order. Upvoting one bundle of a sidechain downvotes its others.
    BOOST_REQUIRE(chain.Connect({Votes({1, 0})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[0].score, 0U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, 2U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(6)->bundles[0].score, 2U);

    // Scores never go below zero.
    BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE, VOTE_ABSTAIN})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[0].score, 0U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(6)->bundles[0].score, 2U);

    // Votes refer to the bundles before the block, whatever new bundle it brings.
    BOOST_REQUIRE(chain.Connect({BundleScript(0, d), Votes({1, VOTE_DOWNVOTE})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, 2U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[2].score, 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(6)->bundles[0].score, 1U);

    // REPEAT_PREVIOUS casts the votes of the last block again; after a block without votes, none.
    BOOST_REQUIRE(chain.Connect({VoteScript({VoteForm::REPEAT_PREVIOUS, {}})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, 3U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(6)->bundles[0].score, 0U);
    BOOST_REQUIRE(chain.Connect());
    BOOST_REQUIRE(chain.Connect({VoteScript({VoteForm::REPEAT_PREVIOUS, {}})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, 3U);

    // There is a limit to the number of pending bundles. A full queue makes room by failing its
    // weakest bundle, if that one has no more than a new bundle's vote: here a, at 0.
    BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(0)->bundles.size(), chain.params.max_pending_bundles);
    BOOST_REQUIRE(chain.Connect({BundleScript(0, uint256{0xe1})}));
    BOOST_CHECK(chain.scdb.WasPaid(0, a) == std::optional<bool>{false});
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles.size(), chain.params.max_pending_bundles);
    BOOST_CHECK(chain.scdb.GetSlot(0)->bundles.back().hash == uint256{0xe1});
    // The weakest is the lowest score, the oldest of those; a bundle with votes above a new one's is never pushed out.
    std::vector<Bundle> queue(3);
    queue[0].score = 5;
    queue[1].score = 2;
    queue[2].score = 2;
    BOOST_CHECK(SidechainDB::WeakestBundle(queue) == queue.begin() + 1);
    BOOST_CHECK(SidechainDB::WeakestBundle(queue)->score > NEW_BUNDLE_SCORE);
}

BOOST_AUTO_TEST_CASE(leading_by_50)
{
    TestChain chain;
    chain.params.withdrawal_min_score = 100;
    chain.params.withdrawal_period = 200;
    chain.Activate(MakeSidechain(0));
    BOOST_REQUIRE(chain.Connect({BundleScript(0, uint256{0xa1})}));
    BOOST_REQUIRE(chain.Connect({BundleScript(0, uint256{0xb1})}));
    const CScript leading{VoteScript({VoteForm::LEADING_BY_50, {}})};

    // Nothing leads by enough yet.
    BOOST_REQUIRE(chain.Connect({leading}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, 1U);
    while (chain.scdb.GetSlot(0)->bundles[1].score < LEADING_BY_50_MARGIN + 1) BOOST_REQUIRE(chain.Connect({Votes({1})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[0].score, 0U);
    BOOST_REQUIRE(chain.Connect({leading}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(0)->bundles[1].score, LEADING_BY_50_MARGIN + 2);
}

BOOST_AUTO_TEST_CASE(withdrawal_expiry)
{
    TestChain chain;
    chain.Activate(MakeSidechain(0));
    const uint256 hash{0xa1};
    BOOST_REQUIRE(chain.Connect({BundleScript(0, hash)}));
    const int proposed{chain.height};

    // Without votes the bundle fails as soon as the blocks left cannot bring it to the minimum score:
    // it needs min_score - 1 more points.
    const int alive{chain.params.withdrawal_period - (chain.params.withdrawal_min_score - 1)};
    while (chain.height < proposed + alive) {
        BOOST_REQUIRE(chain.Connect());
        BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(0)->bundles.size(), 1U);
    }
    BOOST_REQUIRE(chain.Connect());
    BOOST_CHECK(chain.scdb.GetSlot(0)->bundles.empty());
    BOOST_CHECK(chain.scdb.IsClosed(0, hash));
    BOOST_CHECK(chain.scdb.WasPaid(0, hash) == std::optional<bool>{false});
    BOOST_CHECK(!chain.Connect({BundleScript(0, hash)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bundle-closed");

    // A bundle with the score lives for the whole period, then fails if it was not paid out.
    const uint256 voted{0xa2};
    BOOST_REQUIRE(chain.Connect({BundleScript(0, voted)}));
    const int proposed_voted{chain.height};
    for (int i{1}; i < chain.params.withdrawal_min_score; ++i) BOOST_REQUIRE(chain.Connect({Votes({0})}));
    while (chain.height < proposed_voted + chain.params.withdrawal_period - 1) {
        BOOST_REQUIRE(chain.Connect());
        BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(0)->bundles.size(), 1U);
        BOOST_CHECK_EQUAL(SidechainDB::BlocksLeft(chain.scdb.GetSlot(0)->bundles[0], chain.height, chain.params),
                          proposed_voted + chain.params.withdrawal_period - 1 - chain.height);
    }
    BOOST_REQUIRE(chain.Connect());
    BOOST_CHECK(chain.scdb.GetSlot(0)->bundles.empty());
    BOOST_CHECK(chain.scdb.IsClosed(0, voted));
}

BOOST_AUTO_TEST_CASE(replacement)
{
    TestChain chain;
    const Sidechain original{MakeSidechain(2, "original")};
    chain.Activate(original);
    BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(2, 4 * COIN)}));
    BOOST_REQUIRE(chain.Connect({BundleScript(2, uint256{0xa1})}));
    const Ctip ctip{chain.scdb.GetSlot(2)->ctip};

    // Replacing a sidechain takes the longer period.
    const Sidechain replacement{MakeSidechain(2, "replacement")};
    BOOST_REQUIRE(chain.Connect({ProposalScript(replacement)}));
    for (int i{1}; i < chain.params.replacement_period - 1; ++i) {
        BOOST_REQUIRE(chain.Connect({Ack(replacement)}));
        BOOST_CHECK(chain.scdb.GetSlot(2)->sidechain == original);
    }
    BOOST_REQUIRE(chain.Connect({Ack(replacement)}));
    const Slot* slot{chain.scdb.GetSlot(2)};
    BOOST_CHECK(slot->sidechain == replacement);
    // The new sidechain takes over the escrow but not the pending withdrawals.
    BOOST_CHECK(slot->has_ctip && slot->ctip == ctip);
    BOOST_CHECK(slot->bundles.empty());
    BOOST_CHECK(chain.Connect({}, {chain.DepositTx(2, 5 * COIN)}));
}

BOOST_AUTO_TEST_CASE(bmm)
{
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    chain.Activate(MakeSidechain(2));

    const uint256 h1{0x11}, h2{0x22};
    const auto request_tx = [&](SidechainId slot, const uint256& side_hash, const uint256& prev) {
        CMutableTransaction tx{chain.BaseTx()};
        BmmRequest request;
        request.slot = slot;
        request.side_block_hash = side_hash;
        request.prev_main_block_hash = prev;
        tx.vout.emplace_back(0, BmmRequestScript(request));
        return tx;
    };

    // An accept needs no request: the miner may be running the sidechain itself.
    BOOST_CHECK(chain.Connect({BmmAcceptScript(1, h1)}));
    BOOST_CHECK(chain.Connect({BmmAcceptScript(1, h1), BmmAcceptScript(2, h2)}));
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(1, h1), BmmAcceptScript(1, h2)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-bmm-accepts");
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(3, h1)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bmm-inactive-sidechain");
    // Slots this version does not have are ignored.
    BOOST_CHECK(chain.Connect({BmmAcceptScript(9, h1)}));

    // A request needs the matching accept.
    BOOST_CHECK(!chain.Connect({}, {request_tx(1, h1, chain.tip)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bmm-not-accepted");
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(1, h2)}, {request_tx(1, h1, chain.tip)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bmm-not-accepted");
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(2, h1)}, {request_tx(1, h1, chain.tip)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bmm-not-accepted");
    BOOST_CHECK(chain.Connect({BmmAcceptScript(1, h1)}, {request_tx(1, h1, chain.tip)}));

    // A request is bound to the block it was made for.
    const uint256 stale{chain.tip};
    chain.ConnectEmpty(1);
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(1, h1)}, {request_tx(1, h1, stale)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bmm-prev-block");

    // One request per sidechain per block, but several sidechains can be merged mined together.
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(1, h1)}, {request_tx(1, h1, chain.tip), request_tx(1, h1, chain.tip)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-bmm-requests");
    BOOST_CHECK(chain.Connect({BmmAcceptScript(1, h1), BmmAcceptScript(2, h2)}, {request_tx(1, h1, chain.tip), request_tx(2, h2, chain.tip)}));
}

BOOST_AUTO_TEST_CASE(proposal_cap)
{
    // A block's proposals beyond MAX_PROPOSALS_PER_BLOCK are not looked at.
    TestChain chain;
    std::vector<CScript> scripts;
    for (size_t i{0}; i < MAX_PROPOSALS_PER_BLOCK + 5; ++i) scripts.push_back(ProposalScript(MakeSidechain(i % 8, strprintf("p%u", i))));
    BOOST_REQUIRE(chain.Connect(scripts));
    BOOST_CHECK_EQUAL(chain.scdb.GetProposals().size(), MAX_PROPOSALS_PER_BLOCK);
    for (const Proposal& proposal : chain.scdb.GetProposals()) BOOST_CHECK(proposal.hash == proposal.sidechain.GetHash());
}

BOOST_AUTO_TEST_CASE(replacement_fails_pending_bundles)
{
    // The bundles of a sidechain that is replaced fail, so that its software refunds them.
    TestChain chain;
    chain.Activate(MakeSidechain(2, "original"));
    const uint256 hash{0xa1};
    BOOST_REQUIRE(chain.Connect({BundleScript(2, hash)}));
    chain.Activate(MakeSidechain(2, "replacement"));
    BOOST_CHECK(chain.scdb.GetSlot(2)->bundles.empty());
    BOOST_CHECK(chain.scdb.WasPaid(2, hash) == std::optional<bool>{false});
}

BOOST_AUTO_TEST_CASE(fee_script_encoding)
{
    // The fee of a blind withdrawal is OP_RETURN OP_PUSHBYTES_8 <fee> and nothing else: the M6 id depends on it.
    BOOST_CHECK(ParseWithdrawalFeeScript(WithdrawalFeeScript(1000)) == 1000);
    std::vector<unsigned char> fee(8);
    WriteBE64(fee.data(), 1000);
    std::vector<unsigned char> raw{OP_RETURN, OP_PUSHDATA1, 8};
    raw.insert(raw.end(), fee.begin(), fee.end());
    const CScript pushdata1{raw.begin(), raw.end()};
    BOOST_CHECK(!ParseWithdrawalFeeScript(pushdata1));
}

BOOST_AUTO_TEST_CASE(follow_vote)
{
    // A miner upvotes the bundle its sidechain node handed it; a bundle it
    // cannot check, it upvotes only once others put it 50 votes ahead (BIP300 "leading by 50").
    MinerState miner;
    BOOST_REQUIRE(miner.GetDefaultVote() == Vote::Type::UPVOTE);
    const auto bundle = [](uint8_t id, uint32_t score) {
        Bundle b;
        b.hash = uint256{id};
        b.score = score;
        return b;
    };
    // Following is something to opt in to: by default a bundle nobody handed over gets no vote.
    BOOST_CHECK(!miner.GetFollow());
    BOOST_CHECK(miner.ResolveVote(1, {bundle(1, 500)}).type == Vote::Type::ABSTAIN);
    miner.SetFollow(true);
    BOOST_CHECK(miner.ResolveVote(1, {bundle(1, 49)}).type == Vote::Type::ABSTAIN);
    const Vote followed{miner.ResolveVote(1, {bundle(1, 50)})};
    BOOST_CHECK(followed.type == Vote::Type::UPVOTE && followed.bundle == uint256{1});
    BOOST_CHECK(miner.ResolveVote(1, {bundle(1, 80), bundle(2, 31)}).type == Vote::Type::ABSTAIN);
    BOOST_CHECK(miner.ResolveVote(1, {bundle(1, 10), bundle(2, 70)}).bundle == uint256{2});

    // One it was handed comes first, whatever the others do.
    CMutableTransaction blind{TestChain::BlindBundle(COIN, 1000)};
    std::string error;
    const auto handed{miner.AddBundle(1, blind, error)};
    BOOST_REQUIRE_MESSAGE(handed, error);
    const Vote own{miner.ResolveVote(1, {bundle(1, 80), Bundle{*handed, 0, 1}})};
    BOOST_CHECK(own.type == Vote::Type::UPVOTE && own.bundle == *handed);

    // Abstaining by default means abstaining.
    miner.SetDefaultVote(Vote::Type::ABSTAIN);
    BOOST_CHECK(miner.ResolveVote(1, {bundle(1, 80)}).type == Vote::Type::ABSTAIN);
}

namespace {
std::vector<CScript> Scripts(const BlockAdditions& additions)
{
    std::vector<CScript> scripts;
    for (const CTxOut& out : additions.coinbase_outputs) scripts.push_back(out.scriptPubKey);
    return scripts;
}

bool Contains(const std::vector<CScript>& scripts, const CScript& script)
{
    return std::find(scripts.begin(), scripts.end(), script) != scripts.end();
}
} // namespace

BOOST_AUTO_TEST_CASE(self_replacement_ignored)
{
    // Proposing the sidechain a slot already has would replace it with itself and fail every
    // pending bundle: such a proposal is not a proposal.
    TestChain chain;
    const Sidechain sidechain{MakeSidechain(2)};
    chain.Activate(sidechain);
    const uint256 bundle{0xa1};
    BOOST_REQUIRE(chain.Connect({BundleScript(2, bundle)}));
    BOOST_REQUIRE(chain.Connect({ProposalScript(sidechain)}));
    BOOST_CHECK(chain.scdb.GetProposals().empty());
    for (int i{0}; i < chain.params.replacement_period; ++i) BOOST_REQUIRE(chain.Connect({Ack(sidechain)}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(2)->bundles.size(), 1U);
}

BOOST_AUTO_TEST_CASE(acks_are_per_slot)
{
    // An ack of a sidechain in one slot is not an ack of the same sidechain proposed in another,
    // and acks are dropped once the sidechain activates.
    TestChain chain;
    MinerState miner;
    const Sidechain sidechain{MakeSidechain(2)};
    miner.AddProposal(sidechain);
    BOOST_CHECK(miner.IsAcked(2, sidechain.GetHash()));
    chain.Activate(sidechain);
    miner.Prune(chain.scdb, chain.height + 1);
    BOOST_CHECK(!miner.IsAcked(2, sidechain.GetHash()));
    BOOST_CHECK(miner.GetProposals().empty());

    // The same description proposed in slot 3.
    miner.SetAck(2, sidechain.GetHash(), true);
    Sidechain copy{sidechain};
    copy.slot = 3;
    BOOST_REQUIRE(chain.Connect({ProposalScript(copy)}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetProposals().size(), 1U);
    const auto scripts{Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {}))};
    BOOST_CHECK(!Contains(scripts, AckScript(3, copy.GetHash())));
    miner.SetAck(3, copy.GetHash(), true);
    BOOST_CHECK(Contains(Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {})), AckScript(3, copy.GetHash())));
}

BOOST_AUTO_TEST_CASE(upvote_last_handed_bundle)
{
    // A bundle the sidechain node handed before the last one is one its chain gave up: it gets no
    // vote, even with more votes than the one that replaced it.
    MinerState miner;
    std::string error;
    const auto old_bundle{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 1), error)};
    const auto new_bundle{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 2), error)};
    BOOST_REQUIRE(old_bundle && new_bundle);
    const Vote vote{miner.ResolveVote(1, {Bundle{*old_bundle, 0, 50}, Bundle{*new_bundle, 0, 1}})};
    BOOST_CHECK(vote.type == Vote::Type::UPVOTE && vote.bundle == *new_bundle);
    // Not pending yet: no vote for the old one meanwhile.
    BOOST_CHECK(miner.ResolveVote(1, {Bundle{*old_bundle, 0, 50}}).type == Vote::Type::ABSTAIN);
}

BOOST_AUTO_TEST_CASE(vouching)
{
    // The sidechain node says which bundle its chain has, or none: the default vote follows.
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    MinerState miner;
    std::string error;
    const auto mine{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 1), error)};
    const auto stale{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 2), error)};
    BOOST_REQUIRE(mine && stale);
    // A bundle never handed cannot be vouched for.
    BOOST_CHECK(!miner.Vouch(1, uint256{0xee}));
    BOOST_CHECK(!miner.Vouched(2));
    // None: the pending bundles of the slot are downvoted; with none pending, nothing to say.
    BOOST_CHECK(miner.Vouch(1, std::nullopt));
    BOOST_CHECK(miner.Vouched(1) == uint256{});
    BOOST_CHECK(miner.ResolveVote(1, {Bundle{*stale, 0, 40}}).type == Vote::Type::DOWNVOTE);
    BOOST_CHECK(miner.ResolveVote(1, {}).type == Vote::Type::ABSTAIN);
    // A bundle: upvoted, and the only one proposed.
    BOOST_CHECK(miner.Vouch(1, *mine));
    const Vote vote{miner.ResolveVote(1, {Bundle{*stale, 0, 40}, Bundle{*mine, 0, 1}})};
    BOOST_CHECK(vote.type == Vote::Type::UPVOTE && vote.bundle == *mine);
    const auto scripts{Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {}))};
    BOOST_CHECK(Contains(scripts, BundleScript(1, *mine)));
    BOOST_CHECK(!Contains(scripts, BundleScript(1, *stale)));
    // An operator's own vote for the slot still comes first.
    Vote abstain;
    abstain.type = Vote::Type::ABSTAIN;
    miner.SetVote(1, abstain);
    BOOST_CHECK(miner.ResolveVote(1, {Bundle{*mine, 0, 1}}).type == Vote::Type::ABSTAIN);
}

BOOST_AUTO_TEST_CASE(idle_bundles_expire)
{
    // A bundle at score 0 once it is idle_expiry_blocks old fails.
    {
        TestChain chain;
        chain.params.idle_expiry_blocks = 5;
        chain.Activate(MakeSidechain(1));
        const uint256 junk{0x99};
        BOOST_REQUIRE(chain.Connect({BundleScript(1, junk)}));
        BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
        BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles[0].score, 0U);
        // Its age counts the block that proposed it: 4 blocks old after the next two, 5 after the third.
        for (int i{0}; i < 2; ++i) BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
        BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.empty());
        BOOST_CHECK(chain.scdb.WasPaid(1, junk) == std::optional<bool>{false});
    }
    // One with votes stays.
    TestChain chain;
    chain.params.idle_expiry_blocks = 5;
    chain.Activate(MakeSidechain(1));
    BOOST_REQUIRE(chain.Connect({BundleScript(1, uint256{0x98})}));
    for (int i{0}; i < 8; ++i) BOOST_REQUIRE(chain.Connect({Votes({0})}));
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
}

BOOST_AUTO_TEST_CASE(full_queue_bundle_after_own_vote)
{
    // The miner proposes a bundle into a full queue only if the weakest bundle is weak enough
    // after the block's own votes, which count first: otherwise its block would be invalid.
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    MinerState miner;
    miner.SetDefaultVote(Vote::Type::ABSTAIN);
    const uint256 a{0xa1}, b{0xb1}, c{0xc1};
    BOOST_REQUIRE(chain.Connect({BundleScript(1, a)}));
    for (int i{0}; i < 5; ++i) BOOST_REQUIRE(chain.Connect({Votes({0})}));
    BOOST_REQUIRE(chain.Connect({BundleScript(1, b)}));
    for (int i{0}; i < 3; ++i) BOOST_REQUIRE(chain.Connect({Votes({1})}));
    BOOST_REQUIRE(chain.Connect({BundleScript(1, c)}));
    const auto& bundles{chain.scdb.GetSlot(1)->bundles};
    BOOST_REQUIRE_EQUAL(bundles.size(), 3U);
    BOOST_REQUIRE_EQUAL(bundles[2].score, 1U);

    std::string error;
    const auto handed{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 9), error)};
    BOOST_REQUIRE(handed);
    // Upvoting the weakest bundle lifts it above what may be pushed out: no new bundle.
    Vote upvote;
    upvote.type = Vote::Type::UPVOTE;
    upvote.bundle = c;
    miner.SetVote(1, upvote);
    auto scripts{Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {}))};
    BOOST_CHECK(!Contains(scripts, BundleScript(1, *handed)));
    BOOST_CHECK(chain.Connect(scripts));
    // Abstaining, the weakest is pushed out.
    miner.ClearVote(1);
    BOOST_REQUIRE(chain.Connect({Votes({0})}));
    BOOST_REQUIRE(chain.Connect({Votes({0})}));
    scripts = Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {}));
    BOOST_CHECK(Contains(scripts, BundleScript(1, *handed)));
    BOOST_CHECK_MESSAGE(chain.Connect(scripts), chain.reject_reason);
}

BOOST_AUTO_TEST_CASE(treasury_is_never_dust)
{
    // A withdrawal may leave the treasury small, or empty, and must still be relayed.
    BOOST_CHECK(!IsDust(CTxOut{0, EscrowScript(1)}, CFeeRate{DUST_RELAY_TX_FEE}));
    BOOST_CHECK(!IsDust(CTxOut{1, EscrowScript(1)}, CFeeRate{DUST_RELAY_TX_FEE}));
    BOOST_CHECK(IsDust(CTxOut{1, CScript() << OP_TRUE}, CFeeRate{DUST_RELAY_TX_FEE}));
}

BOOST_AUTO_TEST_CASE(many_blocks_undo)
{
    // Rewind a long history block by block and compare with the states it went through.
    TestChain chain;
    std::vector<SidechainDB> states{chain.scdb};
    std::vector<BlockUndo> undos;
    const auto connect = [&](const std::vector<CScript>& scripts, const std::vector<CMutableTransaction>& txs = {}) {
        const CBlock block{chain.MakeBlock(scripts, txs)};
        BlockUndo undo;
        std::string reason;
        BOOST_REQUIRE_MESSAGE(chain.scdb.ConnectBlock(block, chain.height + 1, chain.params, undo, nullptr, reason), reason);
        chain.tip = block.GetHash();
        ++chain.height;
        states.push_back(chain.scdb);
        undos.push_back(undo);
    };

    const Sidechain a{MakeSidechain(1, "a")};
    const Sidechain doomed{MakeSidechain(3, "doomed")};
    connect({ProposalScript(a)});
    connect({ProposalScript(doomed), Ack(a)});
    for (int i{2}; i < chain.params.activation_period; ++i) connect({Ack(a)});
    BOOST_REQUIRE(chain.scdb.IsActive(1));
    BOOST_REQUIRE(chain.scdb.GetProposals().empty());

    connect({}, {chain.DepositTx(1, 10 * COIN)});
    const CMutableTransaction blind{TestChain::BlindBundle(2 * COIN, 1000)};
    connect({BundleScript(1, blind.GetHash().ToUint256())});
    connect({BundleScript(1, uint256{0xf1})});
    for (int i{1}; i < chain.params.withdrawal_min_score; ++i) connect({Votes({0})}, {chain.DepositTx(1, (10 + i) * COIN)});
    connect({}, {chain.WithdrawalTx(1, blind, 2 * COIN, 1000)});
    const Sidechain b{MakeSidechain(1, "b")};
    connect({ProposalScript(b)});
    for (int i{1}; i < chain.params.replacement_period; ++i) connect({Ack(b)});
    connect({VoteScript({VoteForm::REPEAT_PREVIOUS, {}})});
    BOOST_REQUIRE(chain.scdb.GetSlot(1)->sidechain == b);

    while (!undos.empty()) {
        chain.scdb.DisconnectBlock(undos.back());
        undos.pop_back();
        states.pop_back();
        BOOST_REQUIRE(chain.scdb == states.back());
    }
    BOOST_CHECK(chain.scdb.GetSlots().empty());
}

BOOST_AUTO_TEST_CASE(deposit_address_format)
{
    using drivechain::DepositAddress;
    using drivechain::DepositAddressKind;
    using drivechain::FormatDepositAddress;
    using drivechain::ParseDepositAddress;

    // The checksum is the first six hexadecimal digits of the SHA-256 hash of what comes before it.
    BOOST_CHECK_EQUAL(FormatDepositAddress({1, "erin"}), "s1_erin_b91ea9");
    BOOST_CHECK_EQUAL(FormatDepositAddress({3, "alice"}), "s3_alice_7ec2cc");

    DepositAddress parsed;
    BOOST_CHECK(ParseDepositAddress("s3_alice_7ec2cc", parsed) == DepositAddressKind::VALID);
    BOOST_CHECK_EQUAL(parsed.slot, 3U);
    BOOST_CHECK_EQUAL(parsed.address, "alice");
    // An address may have underscores of its own.
    const std::string with_underscores{FormatDepositAddress({511, "a_b__c"})};
    BOOST_CHECK(ParseDepositAddress(with_underscores, parsed) == DepositAddressKind::VALID);
    BOOST_CHECK_EQUAL(parsed.slot, 511U);
    BOOST_CHECK_EQUAL(parsed.address, "a_b__c");

    // Any change is caught: in the slot, in the address, in the checksum.
    BOOST_CHECK(ParseDepositAddress("s4_alice_7ec2cc", parsed) == DepositAddressKind::INVALID);
    BOOST_CHECK(ParseDepositAddress("s3_alicf_7ec2cc", parsed) == DepositAddressKind::INVALID);
    BOOST_CHECK(ParseDepositAddress("s3_alice_7ec2cd", parsed) == DepositAddressKind::INVALID);
    BOOST_CHECK(ParseDepositAddress("s3_alice_7EC2CC", parsed) == DepositAddressKind::INVALID);
    // A slot number that does not fit is no slot.
    BOOST_CHECK(ParseDepositAddress("s99999999999_alice_7ec2cc", parsed) == DepositAddressKind::INVALID);

    // What does not have the form is a destination as it is.
    for (const std::string plain : {"", "alice", "s3", "s3_alice", "s3__7ec2cc", "s_alice_7ec2cc", "sx_alice_7ec2cc", "S3_alice_7ec2cc",
                                    "s3_alice_7ec2c", "s3_alice_7ec2ccc", "rchn1qne4dtvnal6mql3kusvxq74vk9mcj4u2gks3txn"}) {
        BOOST_CHECK_MESSAGE(ParseDepositAddress(plain, parsed) == DepositAddressKind::PLAIN, plain);
    }
}

BOOST_AUTO_TEST_CASE(deposit_script)
{
    SelectParams(ChainType::REGTEST);
    const CScript burn{CScript() << OP_RETURN};
    const CTxDestination dest{WitnessV0KeyHash{uint160{}}};
    const std::string address{EncodeDestination(dest)};
    const CScript pay{GetScriptForDestination(dest)};

    // An address of this chain, as it is or in a deposit address of this sidechain, is paid.
    BOOST_CHECK(sidechain::DepositScript(address, 5) == pay);
    BOOST_CHECK(sidechain::DepositScript(drivechain::FormatDepositAddress({5, address}), 5) == pay);
    // A deposit address of another sidechain is not, whatever address it holds: the coins are burned.
    BOOST_CHECK(sidechain::DepositScript(drivechain::FormatDepositAddress({6, address}), 5) == burn);
    // Nor one with a wrong checksum, or anything that is no address of this chain.
    std::string mistyped{drivechain::FormatDepositAddress({5, address})};
    mistyped.back() = mistyped.back() == '0' ? '1' : '0';
    BOOST_CHECK(sidechain::DepositScript(mistyped, 5) == burn);
    BOOST_CHECK(sidechain::DepositScript(drivechain::FormatDepositAddress({5, "alice"}), 5) == burn);
    BOOST_CHECK(sidechain::DepositScript("alice", 5) == burn);
    // The prefix of regtest is this chain's own: an address of the mainchain is none of this chain.
    BOOST_CHECK(sidechain::DepositScript("rchn1qne4dtvnal6mql3kusvxq74vk9mcj4u2gks3txn", 5) == burn);
}

BOOST_AUTO_TEST_CASE(withdrawal_to_treasury_refused)
{
    // The mainchain refuses bundles that pay into a treasury, so such a withdrawal is no withdrawal.
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{uint160::FromHex("0101010101010101010101010101010101010101").value()})};
    const CTxOut good{COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), pay)};
    BOOST_CHECK(sidechain::ParseWithdrawalOutput(good));
    const CTxOut treasury{COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), EscrowScript(2))};
    BOOST_CHECK(!sidechain::ParseWithdrawalOutput(treasury));
    // Nor any output a mainchain node would not relay: the bundle could not be broadcast.
    for (const CScript& odd : {CScript() << OP_TRUE, CScript() << OP_1 << ToByteVector(CPubKey{}) << OP_1 << OP_CHECKMULTISIG}) {
        BOOST_CHECK(!sidechain::ParseWithdrawalOutput(CTxOut{COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), odd)}));
    }
}

namespace {
/** The state of a sidechain, on a store of its own that journals (as a block's would). */
struct SideStore {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    size_t Withdrawals() const
    {
        size_t n{0};
        state.ForEachWithdrawal([&](const sidechain::Withdrawal&) { ++n; return true; });
        return n;
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(payout_queue)
{
    // A block pays at most MAX_PAYOUTS_PER_BLOCK outputs; the rest wait, in order, and undo restores them.
    SideStore side;
    sidechain::State& state{side.state};
    std::vector<CTxOut> owed;
    for (size_t i{0}; i < sidechain::MAX_PAYOUTS_PER_BLOCK + 500; ++i) owed.emplace_back(static_cast<CAmount>(i + 1), CScript() << OP_TRUE);
    const auto paid1{state.TakePayouts(owed, {})};
    const sidechain::StoreUndo first{side.store.TakeUndo()};
    BOOST_REQUIRE_EQUAL(paid1.size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK(paid1.front() == owed.front() && paid1.back() == owed[sidechain::MAX_PAYOUTS_PER_BLOCK - 1]);
    BOOST_CHECK_EQUAL(state.Queue().size(), 500U);
    const uint256 after_first{state.Hash()};
    const CTxOut extra{7 * COIN, CScript() << OP_TRUE << OP_TRUE};
    const auto paid2{state.TakePayouts({}, {extra})};
    const sidechain::StoreUndo second{side.store.TakeUndo()};
    BOOST_REQUIRE_EQUAL(paid2.size(), 501U);
    BOOST_CHECK(paid2.front() == owed[sidechain::MAX_PAYOUTS_PER_BLOCK] && paid2.back() == extra);
    BOOST_CHECK(state.Queue().empty());

    side.store.Revert(second);
    BOOST_CHECK(state.Hash() == after_first);
    side.store.Revert(first);
    side.store.TakeUndo();
    BOOST_CHECK(state.Queue().empty());

    // Payouts of transactions, however many, do not hold back deposits: those go first.
    std::vector<CTxOut> spam(sidechain::MAX_PAYOUTS_PER_BLOCK + 10, CTxOut{1, CScript() << OP_TRUE});
    BOOST_CHECK_EQUAL(state.TakePayouts({}, spam).size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    side.store.TakeUndo();
    const CTxOut deposit{5 * COIN, CScript() << OP_TRUE << OP_TRUE};
    const auto paid4{state.TakePayouts({deposit}, {})};
    const sidechain::StoreUndo fourth{side.store.TakeUndo()};
    BOOST_REQUIRE_EQUAL(paid4.size(), 11U);
    BOOST_CHECK(paid4.front() == deposit);
    BOOST_CHECK(state.TxQueue().empty());
    side.store.Revert(fourth);
    BOOST_CHECK_EQUAL(state.TxQueue().size(), 10U);
}

BOOST_AUTO_TEST_CASE(payout_queues_share_a_block)
{
    // A flood in one queue does not hold the other back for many blocks. Each has half of a block;
    // what one leaves, the other takes.
    const size_t max{sidechain::MAX_PAYOUTS_PER_BLOCK}, half{max / 2};
    SideStore side;
    sidechain::State& state{side.state};
    std::vector<CTxOut> deposits, others;
    for (size_t i{0}; i < 3 * max; ++i) deposits.emplace_back(static_cast<CAmount>(i + 1), CScript() << OP_TRUE);
    for (size_t i{0}; i < 3 * max; ++i) others.emplace_back(static_cast<CAmount>(i + 1), CScript() << OP_TRUE << OP_TRUE);
    // Both full: half each, deposits first, each queue in order.
    const auto paid{state.TakePayouts(deposits, others)};
    BOOST_REQUIRE_EQUAL(paid.size(), max);
    BOOST_CHECK(std::equal(paid.begin(), paid.begin() + half, deposits.begin()));
    BOOST_CHECK(std::equal(paid.begin() + half, paid.end(), others.begin()));
    BOOST_CHECK_EQUAL(state.Queue().size(), 3 * max - half);
    BOOST_CHECK_EQUAL(state.TxQueue().size(), 3 * max - half);
    side.store.Revert(side.store.TakeUndo());
    side.store.TakeUndo();
    BOOST_CHECK(state.Queue().empty() && state.TxQueue().empty());

    // One deposit behind a flood of other payouts: paid in the first block, not after the flood.
    const auto first{state.TakePayouts({}, others)};
    BOOST_CHECK_EQUAL(first.size(), max);
    const CTxOut deposit{5 * COIN, CScript() << OP_2};
    const auto second{state.TakePayouts({deposit}, {})};
    BOOST_REQUIRE_EQUAL(second.size(), max);
    BOOST_CHECK(second.front() == deposit);
    // What the deposits leave of their half goes to the others: all of the block is used.
    BOOST_CHECK(second[1] == others[max]);
    BOOST_CHECK_EQUAL(state.TxQueue().size(), 3 * max - max - (max - 1));
    side.store.Revert(side.store.TakeUndo());
    side.store.TakeUndo();
    BOOST_CHECK(state.Queue().empty() && state.TxQueue().empty());

    // And the other way: a flood of deposits leaves half of every block to the other queue.
    const auto flood{state.TakePayouts(deposits, {})};
    BOOST_CHECK_EQUAL(flood.size(), max);
    const CTxOut refund{COIN, CScript() << OP_3};
    const auto next{state.TakePayouts({}, {refund})};
    BOOST_REQUIRE_EQUAL(next.size(), max);
    BOOST_CHECK(next.back() == refund);
    BOOST_CHECK(next.front() == deposits[max]);
}

BOOST_AUTO_TEST_CASE(bundle_nonce)
{
    // A bundle carries the hash of the block before the one that commits to it: its hash cannot be known in advance.
    Consensus::SidechainParams params;
    SideStore side;
    sidechain::State& state{side.state};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), GetScriptForDestination(WitnessV0KeyHash{uint160::FromHex("0101010101010101010101010101010101010101").value()})));
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 1, params, payouts, reason), reason);
    const auto a{state.NextBundle(10, uint256{0xa}, params)};
    const auto b{state.NextBundle(10, uint256{0xb}, params)};
    BOOST_REQUIRE(a && b);
    BOOST_CHECK(a->GetHash() != b->GetHash());
    const uint256 prev{0xa};
    BOOST_CHECK(a->vout.back().scriptPubKey == (CScript() << OP_RETURN << std::vector<unsigned char>(prev.begin(), prev.end())));
    BOOST_CHECK(IsBlindWithdrawal(CTransaction{*a}));
    // The pending bundle remembers it, so that the bundle can be built again.
    BOOST_REQUIRE_MESSAGE(state.StartBundle(a->GetHash().ToUint256(), 10, uint256{0xa}, params, reason), reason);
    BOOST_CHECK(state.BundleTx()->GetHash() == a->GetHash());
}

BOOST_AUTO_TEST_CASE(paid_on_another_branch)
{
    // A bundle committed on another branch of this chain, which this branch never had, is paid out on
    // the mainchain: the withdrawals it paid are paid on this branch too, and cannot be paid again.
    Consensus::SidechainParams params;
    SideStore side;
    sidechain::State& state{side.state};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{uint160::FromHex("0101010101010101010101010101010101010101").value()})};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), pay));
    tx.vout.emplace_back(2 * COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), pay));
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 1, params, payouts, reason), reason);
    BOOST_REQUIRE_EQUAL(side.Withdrawals(), 2U);

    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    BOOST_REQUIRE(mainchain.Append(genesis));
    sidechain::MainBlock paying;
    paying.hash = uint256{2};
    paying.prev_hash = genesis.hash;
    sidechain::MainDeposit change;
    change.destination = WITHDRAWAL_RETURN_DEST;
    change.bundle = uint256{0xb};
    change.payouts.emplace_back(COIN - 1000, pay);
    paying.deposits.push_back(change);
    BOOST_REQUIRE(mainchain.Append(paying));

    const uint256 before{state.Hash()};
    side.store.TakeUndo();
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(1, mainchain, 2, params, payouts, reason), reason);
    // The one withdrawal that pays that output with that amount is gone; the other stays.
    BOOST_REQUIRE_EQUAL(side.Withdrawals(), 1U);
    state.ForEachWithdrawal([&](const sidechain::Withdrawal& w) { BOOST_CHECK_EQUAL(w.amount, 2 * COIN - 1000); return true; });
    side.store.Revert(side.store.TakeUndo());
    BOOST_CHECK(state.Hash() == before);
}

BOOST_AUTO_TEST_CASE(paid_on_another_branch_oldest_first)
{
    // Of several withdrawals that pay the same output with the same amount, a payout of a bundle of
    // another branch takes the oldest, whatever their outpoints.
    Consensus::SidechainParams params;
    SideStore side;
    sidechain::State& state{side.state};
    const uint160 key{uint160::FromHex("0101010101010101010101010101010101010101").value()};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key})};
    std::vector<CTxOut> payouts;
    std::string reason;
    const auto withdraw{[&](uint8_t salt, int height) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{salt}), 0};
        tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, key, pay));
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, height, params, payouts, reason), reason);
        return COutPoint{tx.GetHash(), 0};
    }};
    const COutPoint older{withdraw(1, 1)}, newer{withdraw(2, 2)};
    // Whichever sorts first by outpoint, the older one goes.
    sidechain::Mainchain mainchain;
    sidechain::MainBlock genesis;
    genesis.hash = uint256{1};
    BOOST_REQUIRE(mainchain.Append(genesis));
    sidechain::MainBlock paying;
    paying.hash = uint256{2};
    paying.prev_hash = genesis.hash;
    sidechain::MainDeposit change;
    change.destination = WITHDRAWAL_RETURN_DEST;
    change.bundle = uint256{0xb};
    change.payouts.emplace_back(COIN - 1000, pay);
    paying.deposits.push_back(change);
    BOOST_REQUIRE(mainchain.Append(paying));
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(1, mainchain, 3, params, payouts, reason), reason);
    BOOST_REQUIRE_EQUAL(side.Withdrawals(), 1U);
    BOOST_CHECK(state.GetWithdrawal(newer).has_value());
    BOOST_CHECK(!state.GetWithdrawal(older).has_value());
}

namespace {
/** A withdrawal of COIN (less a fee of 1000) to `pay`, refundable by `key`, made at `height`. */
COutPoint MakeWithdrawal(sidechain::State& state, const CKey& key, const CScript& pay, int height, const Consensus::SidechainParams& params, uint8_t salt = 0)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{salt}), 0};
    tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), pay));
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, height, params, payouts, reason), reason);
    return COutPoint{tx.GetHash(), 0};
}

sidechain::RefundRequest SignRefund(const CKey& key, const COutPoint& withdrawal)
{
    sidechain::RefundRequest refund;
    refund.withdrawal = withdrawal;
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(withdrawal)), refund.signature));
    return refund;
}

/** A mainchain record of `count` empty blocks. */
void Extend(sidechain::Mainchain& mainchain, int count, const std::function<void(sidechain::MainBlock&)>& fill = {})
{
    for (int i{0}; i < count; ++i) {
        sidechain::MainBlock block;
        const int height{mainchain.Height() + 1};
        block.hash = uint256{static_cast<uint8_t>(height + 1)};
        block.hash.data()[30] = 0x5a;
        block.prev_hash = mainchain.TipHash();
        if (fill) fill(block);
        BOOST_REQUIRE(mainchain.Append(block));
    }
}
} // namespace

BOOST_AUTO_TEST_CASE(pending_bundle_holds_refunds_back_only_with_support)
{
    // The double payout: a bundle committed on another branch of this chain is pending on the
    // mainchain. This branch never had it, so the withdrawals in it look free here: refunded here and
    // then paid there, a withdrawal would be paid twice. Such a bundle holds refunds back with a score
    // of pending_min_score or more, and never holds a new bundle back (the mainchain pays one of the
    // two). Anyone can propose a bundle (a bare M3); miners who vouch for this chain's bundle downvote
    // it, and it stays near 0.
    Consensus::SidechainParams params;
    params.pending_min_score = 3;
    SideStore side;
    sidechain::State& state{side.state};
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey().GetID()})};
    const COutPoint withdrawal{MakeWithdrawal(state, key, pay, 1, params)};
    const auto refund{SignRefund(key, withdrawal)};
    std::string reason;
    std::vector<CTxOut> payouts;

    const uint256 foreign{0xf0};
    sidechain::Mainchain mainchain;
    Extend(mainchain, 1);
    // Proposed: score 1, then 2, then 3.
    for (uint32_t score{1}; score <= 3; ++score) Extend(mainchain, 1, [&](sidechain::MainBlock& b) {
        if (score == 1) b.proposed.push_back(foreign);
        b.pending.push_back({foreign, score});
    });
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(2, mainchain, 2, params, payouts, reason), reason);
    // Score 2: no support yet. The refund goes, and so would a bundle.
    BOOST_CHECK(!state.MainPending(mainchain, params));
    BOOST_CHECK(state.CheckRefund(refund, reason, state.MainPending(mainchain, params)));
    BOOST_CHECK(state.NextBundle(2, uint256{0xa}, params));
    // Score 3: refunds wait, but a bundle of this branch can start: the mainchain pays one of the two.
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(3, mainchain, 3, params, payouts, reason), reason);
    const bool pending{state.MainPending(mainchain, params)};
    BOOST_CHECK(pending);
    BOOST_CHECK(!state.CheckRefund(refund, reason, pending));
    BOOST_CHECK_EQUAL(reason, "bad-sc-refund-bundle-pending");
    BOOST_CHECK(state.MainPendingNext(mainchain, params));
    const auto ours{state.NextBundle(3, uint256{0xa}, params)};
    BOOST_REQUIRE(ours);
    BOOST_REQUIRE_MESSAGE(state.StartBundle(ours->GetHash().ToUint256(), 3, uint256{0xa}, params, reason), reason);
    BOOST_CHECK_EQUAL(state.BundleMainHeight(), 4);
    // This chain's own bundle, however supported, holds back only its own withdrawals.
    Extend(mainchain, 1, [&](sidechain::MainBlock& b) {
        b.proposed.push_back(ours->GetHash().ToUint256());
        b.pending.push_back({foreign, 0});
        b.pending.push_back({ours->GetHash().ToUint256(), 40});
    });
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(4, mainchain, 4, params, payouts, reason), reason);
    BOOST_CHECK(!state.MainPending(mainchain, params));
    BOOST_CHECK(!state.CheckRefund(refund, reason, false));
    BOOST_CHECK_EQUAL(reason, "bad-sc-refund-in-bundle");
}

BOOST_AUTO_TEST_CASE(rising_leader_holds_refunds_back)
{
    // Mainchain miners in follow mode upvote whichever bundle leads its slot, so a
    // bundle of another branch below pending_min_score can still be paid. While it leads with a score
    // on the rise (PENDING_TREND_MIN_RISE over PENDING_TREND_BLOCKS), refunds wait; a bare proposal
    // nobody upvotes, one on its way down, one that does not lead, or a tie, do not hold them back.
    using sidechain::PENDING_TREND_BLOCKS;
    using sidechain::PENDING_TREND_MIN_RISE;
    Consensus::SidechainParams params;
    params.pending_min_score = 1000;
    SideStore side;
    sidechain::State& state{side.state};
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey().GetID()})};
    const COutPoint withdrawal{MakeWithdrawal(state, key, pay, 1, params)};
    const auto refund{SignRefund(key, withdrawal)};
    const uint256 foreign{0xf0}, other{0xf1};
    // The record after each block, as the scores of the pending bundles; refunds held back after it.
    const auto check{[&](const std::vector<std::vector<sidechain::MainPendingBundle>>& blocks, bool held, const std::string& what) {
        sidechain::Mainchain mainchain;
        for (const auto& pending : blocks) Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.pending = pending; });
        const int top{mainchain.Height()};
        // The state has followed the mainchain up to the last block.
        SideStore copy;
        sidechain::State& st{copy.state};
        MakeWithdrawal(st, key, pay, 1, params);
        std::vector<CTxOut> payouts;
        std::string reason;
        BOOST_REQUIRE_MESSAGE(st.ApplyMainEvents(top, mainchain, 2, params, payouts, reason), reason);
        BOOST_CHECK_MESSAGE(st.MainPending(mainchain, params) == held, what);
        BOOST_CHECK_MESSAGE(st.MainPendingNext(mainchain, params) == held, what);
        BOOST_CHECK_MESSAGE(st.CheckRefund(refund, reason, st.MainPending(mainchain, params)) == !held, what);
    }};
    // Upvoted every block: from the proposal (1) up by PENDING_TREND_MIN_RISE, it holds refunds back.
    std::vector<std::vector<sidechain::MainPendingBundle>> rising;
    for (uint32_t score{1}; score <= 1 + PENDING_TREND_MIN_RISE; ++score) rising.push_back({{foreign, score}});
    check(rising, true, "rising");
    // One short of the rise: not yet.
    check({rising.begin(), rising.end() - 1}, false, "one short");
    // A bare proposal nobody votes on stays at 1 for ever.
    check(std::vector<std::vector<sidechain::MainPendingBundle>>(2 * PENDING_TREND_BLOCKS, {{foreign, 1}}), false, "bare");
    // Upvoted to 40, then downvoted: on its way down, below pending_min_score.
    std::vector<std::vector<sidechain::MainPendingBundle>> down;
    for (uint32_t score{1}; score <= 40; ++score) down.push_back({{foreign, score}});
    for (uint32_t score{39}; score >= 30; --score) down.push_back({{foreign, score}});
    check(down, false, "decaying");
    // Upvoted in a quarter of the blocks, no downvotes: rising at the least pace that counts.
    std::vector<std::vector<sidechain::MainPendingBundle>> slow;
    for (int i{0}; i < 4 * PENDING_TREND_BLOCKS; ++i) slow.push_back({{foreign, static_cast<uint32_t>(1 + i / 4)}});
    check(slow, true, "slow");
    // Upvoted in fewer blocks: on its way to failing.
    std::vector<std::vector<sidechain::MainPendingBundle>> slower;
    for (int i{0}; i < 5 * PENDING_TREND_BLOCKS; ++i) slower.push_back({{foreign, static_cast<uint32_t>(1 + i / 5)}});
    check(slower, false, "slower");
    // Rising, but another bundle leads: follow-mode miners upvote that one.
    std::vector<std::vector<sidechain::MainPendingBundle>> behind;
    for (uint32_t score{1}; score <= 10; ++score) behind.push_back({{foreign, score}, {other, 50}});
    check(behind, false, "behind");
    // A tie leads nobody.
    std::vector<std::vector<sidechain::MainPendingBundle>> tie;
    for (uint32_t score{1}; score <= 10; ++score) tie.push_back({{foreign, score}, {other, score}});
    check(tie, false, "tie");
    // The other leads and rises: it holds refunds back, whichever it is.
    std::vector<std::vector<sidechain::MainPendingBundle>> ahead;
    for (uint32_t score{1}; score <= 10; ++score) ahead.push_back({{foreign, 1}, {other, score}});
    check(ahead, true, "ahead");

    // This chain's own bundle, leading and rising, holds back only its own withdrawals.
    sidechain::Mainchain mainchain;
    Extend(mainchain, 1);
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(0, mainchain, 2, params, payouts, reason), reason);
    const auto ours{state.NextBundle(2, uint256{0xa}, params)};
    BOOST_REQUIRE(ours);
    const uint256 ours_hash{ours->GetHash().ToUint256()};
    BOOST_REQUIRE_MESSAGE(state.StartBundle(ours_hash, 2, uint256{0xa}, params, reason), reason);
    for (uint32_t score{1}; score <= 10; ++score) Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.pending = {{ours_hash, score}, {foreign, 1}}; });
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(mainchain.Height(), mainchain, 3, params, payouts, reason), reason);
    BOOST_CHECK(!state.MainPending(mainchain, params));
}

BOOST_AUTO_TEST_CASE(foreign_bundle_paid_while_ours_pending)
{
    // Both pending, with the same withdrawal: the mainchain pays the other branch's bundle and fails
    // this one (single payout). The withdrawal is paid once; this branch takes it as paid.
    Consensus::SidechainParams params;
    SideStore side;
    sidechain::State& state{side.state};
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey().GetID()})};
    const COutPoint withdrawal{MakeWithdrawal(state, key, pay, 1, params)};
    std::string reason;
    std::vector<CTxOut> payouts;
    const uint256 foreign{0xf0};
    for (const bool ours_proposed : {true, false}) {
        SideStore copy;
        sidechain::State& st{copy.state};
        MakeWithdrawal(st, key, pay, 1, params);
        sidechain::Mainchain mainchain;
        Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.proposed.push_back(foreign); b.pending.push_back({foreign, 1}); });
        BOOST_REQUIRE_MESSAGE(st.ApplyMainEvents(0, mainchain, 2, params, payouts, reason), reason);
        const auto bundle{st.NextBundle(2, uint256{0xa}, params)};
        BOOST_REQUIRE(bundle);
        const uint256 ours{bundle->GetHash().ToUint256()};
        BOOST_REQUIRE_MESSAGE(st.StartBundle(ours, 2, uint256{0xa}, params, reason), reason);
        Extend(mainchain, 1, [&](sidechain::MainBlock& b) {
            if (ours_proposed) b.proposed.push_back(ours);
        });
        // The other is paid: this one fails with it (if it was proposed), and the change of the payout
        // says what it paid.
        Extend(mainchain, 1, [&](sidechain::MainBlock& b) {
            b.bundles.push_back({foreign, true});
            if (ours_proposed) b.bundles.push_back({ours, false});
            sidechain::MainDeposit change;
            change.destination = WITHDRAWAL_RETURN_DEST;
            change.bundle = foreign;
            change.payouts.emplace_back(COIN - 1000, pay);
            b.deposits.push_back(change);
        });
        BOOST_REQUIRE_MESSAGE(st.ApplyMainEvents(2, mainchain, 3, params, payouts, reason), reason);
        BOOST_CHECK(!st.Bundle());
        BOOST_CHECK_EQUAL(copy.Withdrawals(), 0U);
        BOOST_CHECK(!st.GetWithdrawal(withdrawal));
        BOOST_CHECK_EQUAL(st.BundleMainHeight(), -1);
    }
}

BOOST_AUTO_TEST_CASE(unproposed_bundle_fails)
{
    // A bundle the mainchain does not propose unproposed_expiry_blocks blocks after the block that
    // committed to it fails, and its withdrawals can be refunded or bundled again.
    for (const bool proposed : {false, true}) {
        {
            Consensus::SidechainParams params;
            params.unproposed_expiry_blocks = 3;
            params.bundle_retry_delay = 0;
            SideStore side;
            sidechain::State& state{side.state};
            CKey key;
            key.MakeNewKey(/*fCompressed=*/true);
            const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey().GetID()})};
            MakeWithdrawal(state, key, pay, 1, params);
            sidechain::Mainchain mainchain;
            Extend(mainchain, 1);
            std::string reason;
            std::vector<CTxOut> payouts;
            BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(0, mainchain, 2, params, payouts, reason), reason);
            const auto bundle{state.NextBundle(2, uint256{0xa}, params)};
            BOOST_REQUIRE(bundle);
            BOOST_REQUIRE_MESSAGE(state.StartBundle(bundle->GetHash().ToUint256(), 2, uint256{0xa}, params, reason), reason);
            // Committed in mainchain block 1; proposed (or not) in block 3.
            Extend(mainchain, 5, [&](sidechain::MainBlock& b) {
                if (proposed && mainchain.Height() + 1 == 3) b.proposed.push_back(bundle->GetHash().ToUint256());
            });
            // Up to block 3, one short of the limit: pending either way.
            BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(3, mainchain, 3, params, payouts, reason), reason);
            BOOST_CHECK(state.Bundle());
            BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(5, mainchain, 4, params, payouts, reason), reason);
            const bool fails{!proposed};
            BOOST_CHECK_EQUAL(state.Bundle().has_value(), !fails);
            BOOST_CHECK_EQUAL(state.LastFailureHeight(), fails ? 4 : -1);
            if (fails) BOOST_CHECK(state.NextBundle(5, uint256{0xb}, params));
        }
    }
}

BOOST_AUTO_TEST_CASE(pending_after_payout_is_said)
{
    // The rules take for granted that the mainchain pays one bundle per slot and fails the others. A record that shows a payout with another bundle left pending is said loudly.
    Consensus::SidechainParams params;
    SideStore side;
    std::string reason;
    std::vector<CTxOut> payouts;
    const uint256 a{0xa1}, b{0xb1}, c{0xc1};
    sidechain::Mainchain mainchain;
    Extend(mainchain, 1, [&](sidechain::MainBlock& blk) { blk.proposed = {a, b}; blk.pending = {{a, 1}, {b, 1}}; });
    // A paid, B left pending (C, proposed in the same block after the payout, is no matter).
    Extend(mainchain, 1, [&](sidechain::MainBlock& blk) { blk.bundles = {{a, true}}; blk.proposed = {c}; blk.pending = {{b, 1}, {c, 1}}; });
    {
        ASSERT_DEBUG_LOG("paid a withdrawal bundle of this sidechain and left another one pending");
        BOOST_REQUIRE_MESSAGE(side.state.ApplyMainEvents(1, mainchain, 1, params, payouts, reason), reason);
    }
}

BOOST_AUTO_TEST_CASE(bundle_proposed_before_its_commitment)
{
    // The hash of a bundle can be worked out before the block that commits to it (public withdrawals,
    // and the block before). A mainchain miner who proposes it ahead -- the mainchain then refuses a
    // second proposal of it -- does not make it fail as unproposed. A proposal that the mainchain
    // closed before the commitment does not count.
    for (const bool closed_before : {false, true}) {
        Consensus::SidechainParams params;
        params.unproposed_expiry_blocks = 3;
        params.bundle_retry_delay = 0;
        SideStore side;
        sidechain::State& state{side.state};
        CKey key;
        key.MakeNewKey(/*fCompressed=*/true);
        const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey().GetID()})};
        MakeWithdrawal(state, key, pay, 1, params);
        std::string reason;
        std::vector<CTxOut> payouts;
        // Worked out ahead of the sidechain block at height 2, on top of block 0xa.
        const auto bundle{state.NextBundle(2, uint256{0xa}, params)};
        BOOST_REQUIRE(bundle);
        const uint256 hash{bundle->GetHash().ToUint256()};
        sidechain::Mainchain mainchain;
        Extend(mainchain, 1);
        // Mainchain block 1 proposes it; block 2 closes it, or not.
        Extend(mainchain, 1, [&](sidechain::MainBlock& b) {
            b.proposed.push_back(hash);
            b.pending.push_back({hash, 1});
        });
        Extend(mainchain, 1, [&](sidechain::MainBlock& b) {
            if (closed_before) {
                b.bundles.push_back({hash, false});
            } else {
                b.pending.push_back({hash, 2});
            }
        });
        // The sidechain block, committed in mainchain block 3, commits to the bundle.
        BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(2, mainchain, 2, params, payouts, reason), reason);
        BOOST_REQUIRE_MESSAGE(state.StartBundle(hash, 2, uint256{0xa}, params, reason), reason);
        BOOST_CHECK_EQUAL(state.BundleMainHeight(), 3);
        BOOST_CHECK(mainchain.ProposedSince(hash, 3, 10) == !closed_before);
        // Nothing proposes it after the commitment, well past unproposed_expiry_blocks.
        Extend(mainchain, 6);
        BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(8, mainchain, 3, params, payouts, reason), reason);
        BOOST_CHECK_EQUAL(state.Bundle().has_value(), !closed_before);
        BOOST_CHECK_EQUAL(state.LastFailureHeight(), closed_before ? 3 : -1);
    }
}

BOOST_AUTO_TEST_CASE(every_proposal_and_close_counts)
{
    // A bundle closed, forgotten by the mainchain and proposed again: each proposal and each close is
    // on record.
    sidechain::Mainchain mainchain;
    const uint256 x{0x77};
    Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.proposed.push_back(x); });
    Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.bundles.push_back({x, false}); });
    Extend(mainchain, 1);
    Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.proposed.push_back(x); });
    Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.bundles.push_back({x, false}); b.proposed.push_back(x); });
    Extend(mainchain, 1, [&](sidechain::MainBlock& b) { b.bundles.push_back({x, true}); });
    BOOST_CHECK(mainchain.ClosedHeight(x) == 1);
    BOOST_CHECK(mainchain.ProposedBetween(x, 1, 3));
    BOOST_CHECK(!mainchain.ProposedBetween(x, 1, 2));
    BOOST_CHECK(mainchain.ProposedBetween(x, 4, 4));
    // Truncated, the later events go, the earlier stay.
    mainchain.Truncate(3);
    BOOST_CHECK(mainchain.ProposedBetween(x, 3, 3));
    BOOST_CHECK(!mainchain.ProposedBetween(x, 4, 10));
    BOOST_CHECK(mainchain.ClosedHeight(x) == 1);
    mainchain.Truncate(2);
    BOOST_CHECK(!mainchain.ProposedBetween(x, 1, 10));
    BOOST_CHECK(mainchain.ClosedHeight(x) == 1);
}

BOOST_AUTO_TEST_CASE(record_independent_failures)
{
    BOOST_CHECK(sidechain::RecordIndependent("bad-sc-withdrawal-amount"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-sc-refund-signature"));
    BOOST_CHECK(!sidechain::RecordIndependent("bmm-unknown"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-sc-refund-bundle-pending"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-sc-refund-unknown"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-sc-payout"));
}

BOOST_AUTO_TEST_CASE(events_before_the_slot_activated_are_left_out)
{
    // A slot used before by another sidechain: what the mainchain did for that one, up to the block
    // that activated this one, is not this chain's.
    Consensus::SidechainParams params;
    params.main_activation_height = 2;
    sidechain::Mainchain mainchain;
    for (int h{0}; h <= 3; ++h) {
        sidechain::MainBlock block;
        block.hash = uint256{static_cast<uint8_t>(h + 1)};
        if (h > 0) block.prev_hash = uint256{static_cast<uint8_t>(h)};
        sidechain::MainDeposit deposit;
        deposit.destination = "anything";
        deposit.amount = (h + 1) * COIN;
        deposit.txid = uint256{static_cast<uint8_t>(0x10 + h)};
        block.deposits.push_back(deposit);
        BOOST_REQUIRE(mainchain.Append(block));
    }
    std::string reason;
    {
        // Only what came after the activation block (height 3).
        SideStore side;
        std::vector<CTxOut> payouts;
        BOOST_REQUIRE_MESSAGE(side.state.ApplyMainEvents(3, mainchain, 1, params, payouts, reason), reason);
        BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
        BOOST_CHECK_EQUAL(payouts[0].nValue, 4 * COIN);
        BOOST_CHECK_EQUAL(side.state.MainHeight(), 3);
        // A block that stops before the activation applies nothing, and the next goes on from there.
        SideStore early;
        payouts.clear();
        BOOST_REQUIRE_MESSAGE(early.state.ApplyMainEvents(1, mainchain, 1, params, payouts, reason), reason);
        BOOST_CHECK(payouts.empty());
        BOOST_REQUIRE_MESSAGE(early.state.ApplyMainEvents(3, mainchain, 2, params, payouts, reason), reason);
        BOOST_REQUIRE_EQUAL(payouts.size(), 1U);
    }
    {
        // Without an activation height (0), only the first block of the mainchain is left out: no
        // sidechain was active there.
        params.main_activation_height = 0;
        SideStore side;
        std::vector<CTxOut> payouts;
        BOOST_REQUIRE_MESSAGE(side.state.ApplyMainEvents(3, mainchain, 1, params, payouts, reason), reason);
        BOOST_CHECK_EQUAL(payouts.size(), 3U);
    }
}

BOOST_AUTO_TEST_CASE(record_keeps_what_the_follower_needs)
{
    // The sidechain found in the slot and why blocks failed survive a restart.
    const DBParams params{.path = m_args.GetDataDirBase() / "mainchain_record", .cache_bytes = 1 << 20};
    const uint256 block_a{0xa1}, block_b{0xb2};
    {
        sidechain::Mainchain record{params};
        BOOST_CHECK(!record.GetSlotIdentity());
        record.SetSlotIdentity({7, uint256{0x77}});
        record.NoteFailure(block_a, sidechain::Mainchain::Failure::RECORD);
        record.NoteFailure(block_b, sidechain::Mainchain::Failure::MANUAL);
    }
    {
        sidechain::Mainchain record{params};
        BOOST_CHECK(record.GetSlotIdentity() == (sidechain::SlotIdentity{7, uint256{0x77}}));
        BOOST_CHECK(record.GetFailure(block_a) == sidechain::Mainchain::Failure::RECORD);
        BOOST_CHECK(record.GetFailure(block_b) == sidechain::Mainchain::Failure::MANUAL);
        record.ForgetFailure(block_a);
    }
    sidechain::Mainchain record{params};
    BOOST_CHECK(!record.GetFailure(block_a));
    BOOST_CHECK(record.GetFailure(block_b) == sidechain::Mainchain::Failure::MANUAL);
}

BOOST_AUTO_TEST_CASE(record_format)
{
    // A block on record round-trips, with the bundles it proposed and those pending after it.
    sidechain::MainBlock block;
    block.hash = uint256{7};
    block.bundles.push_back({uint256{0xb}, true});
    block.proposed.push_back(uint256{0xc});
    block.pending.push_back({uint256{0xc}, 12});
    DataStream current{};
    current << block;
    sidechain::MainBlock read;
    current >> read;
    BOOST_CHECK(read == block);

    // A record kept on disk loads again.
    const DBParams params{.path = m_args.GetDataDirBase() / "mainchain_format", .cache_bytes = 1 << 20};
    {
        sidechain::Mainchain record{params};
        sidechain::MainBlock first;
        first.hash = uint256{1};
        first.proposed.push_back(uint256{0xd});
        first.pending.push_back({uint256{0xd}, 5});
        BOOST_REQUIRE(record.Append(first));
    }
    {
        sidechain::Mainchain record{params};
        BOOST_CHECK_EQUAL(record.Height(), 0);
        BOOST_CHECK(record.SupportedPending(0, 5, uint256{}));
        BOOST_CHECK(!record.SupportedPending(0, 6, uint256{}));
        BOOST_CHECK(!record.SupportedPending(0, 5, uint256{0xd}));
        // Above the record: as after its last block.
        BOOST_CHECK(record.SupportedPending(7, 5, uint256{}));
        BOOST_CHECK(!record.SupportedPending(-1, 0, uint256{}));
    }
    // One an older release wrote, with blocks, is refused: its blocks lack what the rules need.
    CDBWrapper{params}.Write(uint8_t{'v'}, uint32_t{2});
    BOOST_CHECK_THROW(sidechain::Mainchain{params}, std::runtime_error);
    // An empty one is taken, in the current format.
    const DBParams empty{.path = m_args.GetDataDirBase() / "mainchain_format_empty", .cache_bytes = 1 << 20};
    CDBWrapper{empty}.Write(uint8_t{'v'}, uint32_t{2});
    {
        sidechain::Mainchain record{empty};
        BOOST_CHECK_EQUAL(record.Height(), -1);
    }
    uint32_t version{0};
    BOOST_CHECK(CDBWrapper{empty}.Read(uint8_t{'v'}, version));
    BOOST_CHECK_EQUAL(version, 3U);
}

BOOST_AUTO_TEST_CASE(duplicate_commitment_survives_reorg)
{
    // A sidechain block committed to twice keeps its first commitment when the mainchain drops the
    // block of the second: the follower only takes for lost what is no longer on record at all.
    sidechain::Mainchain mainchain;
    const uint256 side{0x5};
    sidechain::MainBlock block;
    block.hash = uint256{1};
    BOOST_REQUIRE(mainchain.Append(block));
    sidechain::MainBlock first{};
    first.hash = uint256{2};
    first.prev_hash = uint256{1};
    first.bmm = side;
    BOOST_REQUIRE(mainchain.Append(first));
    sidechain::MainBlock again{};
    again.hash = uint256{3};
    again.prev_hash = uint256{2};
    again.bmm = side;
    BOOST_REQUIRE(mainchain.Append(again));
    BOOST_CHECK(mainchain.CommittedHeight(side) == 1);
    const auto removed{mainchain.Truncate(1)};
    BOOST_REQUIRE_EQUAL(removed.size(), 1U);
    BOOST_CHECK(removed[0].bmm == side);
    BOOST_CHECK(mainchain.CommittedHeight(side) == 1);
    // Dropping the first as well loses it.
    mainchain.Truncate(0);
    BOOST_CHECK(!mainchain.CommittedHeight(side));
    // An assumed commitment is not on record.
    sidechain::Mainchain::AssumeCommitted assume{mainchain, 0};
    BOOST_CHECK(mainchain.BmmHeight(side) == 0);
    BOOST_CHECK(!mainchain.CommittedHeight(side));
}

BOOST_AUTO_TEST_CASE(full_queue_rejects)
{
    // A full queue makes room only by failing a bundle no more voted for than a new one: when every
    // pending bundle has votes, a new bundle is refused.
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    BOOST_REQUIRE_EQUAL(chain.params.max_pending_bundles, 3U);
    const uint256 a{0xa1}, b{0xb1}, c{0xc1}, d{0xd1};
    const auto score{[&](size_t i) { return chain.scdb.GetSlot(1)->bundles[i].score; }};
    // Upvoting one bundle lowers the others: a reaches 6, then b 4 (a 3), then c 2 (a 2, b 3).
    BOOST_REQUIRE(chain.Connect({BundleScript(1, a)}));
    for (int i{0}; i < 5; ++i) BOOST_REQUIRE(chain.Connect({Votes({0})}));
    BOOST_REQUIRE(chain.Connect({BundleScript(1, b)}));
    for (int i{0}; i < 3; ++i) BOOST_REQUIRE(chain.Connect({Votes({1})}));
    BOOST_REQUIRE(chain.Connect({BundleScript(1, c)}));
    BOOST_REQUIRE(chain.Connect({Votes({2})}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 3U);
    BOOST_CHECK_EQUAL(score(0), 2U);
    BOOST_CHECK_EQUAL(score(1), 3U);
    BOOST_CHECK_EQUAL(score(2), 2U);
    BOOST_CHECK(!chain.Connect({BundleScript(1, d)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-too-many-bundles");
    // Once the weakest is down to a new bundle's score, the new one takes its place.
    BOOST_REQUIRE(chain.Connect({Votes({1})}));
    BOOST_CHECK_EQUAL(score(0), 1U);
    BOOST_REQUIRE(chain.Connect({BundleScript(1, d)}));
    BOOST_CHECK(chain.scdb.WasPaid(1, a) == std::optional<bool>{false});
    BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.back().hash == d);
}

BOOST_AUTO_TEST_CASE(escrow_inputs_of_two_sidechains)
{
    // One transaction cannot spend the treasuries of two sidechains, merging them.
    TestChain chain;
    chain.Activate(MakeSidechain(2));
    chain.Activate(MakeSidechain(5));
    BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(2, 10 * COIN), chain.DepositTx(5, 10 * COIN)}));
    CMutableTransaction merge{chain.BaseTx()};
    merge.vin.emplace_back(chain.scdb.GetSlot(2)->ctip.outpoint);
    merge.vin.emplace_back(chain.scdb.GetSlot(5)->ctip.outpoint);
    merge.vout.emplace_back(25 * COIN, EscrowScript(2));
    merge.vout.emplace_back(0, DestinationScript("dest"));
    BOOST_CHECK(!chain.Connect({}, {merge}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-multiple-escrow-inputs");
}

BOOST_AUTO_TEST_CASE(withdrawal_paying_more_than_the_treasury)
{
    // A withdrawal whose payouts and new treasury add up to more than the old treasury is no
    // withdrawal (blocks refuse it earlier, as spending more than its inputs).
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(1, 10 * COIN)}));
    const CMutableTransaction blind{TestChain::BlindBundle(3 * COIN, COIN / 10)};
    BOOST_REQUIRE(chain.Connect({BundleScript(1, blind.GetHash().ToUint256())}));
    while (chain.scdb.GetSlot(1)->bundles[0].score < static_cast<uint32_t>(chain.params.withdrawal_min_score)) BOOST_REQUIRE(chain.Connect({Votes({0})}));
    CMutableTransaction tx{chain.WithdrawalTx(1, blind, 3 * COIN, COIN / 10)};
    tx.vout[0].nValue = 9 * COIN; // below the treasury of 10, but with the payout of 3 more than 10 leaves
    BOOST_CHECK(!chain.Connect({}, {tx}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-amount");
}

namespace {
/** Two bundles of slot 1 pending, both with the score: the same withdrawals put in two bundles. */
struct TwoBundles {
    TestChain chain;
    CMutableTransaction first{TestChain::BlindBundle(2 * COIN, COIN / 10, 1)};
    CMutableTransaction second{TestChain::BlindBundle(2 * COIN, COIN / 10, 2)};

    TwoBundles()
    {
        chain.Activate(MakeSidechain(1));
        BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(1, 10 * COIN)}));
        chain.params.withdrawal_min_score = 2;
        // Upvoting one bundle lowers the other: the second to 4 (the first to 0), then the first to 2 (the second to 2).
        BOOST_REQUIRE(chain.Connect({BundleScript(1, first.GetHash().ToUint256())}));
        BOOST_REQUIRE(chain.Connect({BundleScript(1, second.GetHash().ToUint256())}));
        for (int i{0}; i < 3; ++i) BOOST_REQUIRE(chain.Connect({Votes({1})}));
        for (int i{0}; i < 2; ++i) BOOST_REQUIRE(chain.Connect({Votes({0})}));
        BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 2U);
        BOOST_REQUIRE_GE(chain.scdb.GetSlot(1)->bundles[0].score, 2U);
        BOOST_REQUIRE_GE(chain.scdb.GetSlot(1)->bundles[1].score, 2U);
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(one_withdrawal_per_sidechain_per_block)
{
    // A sidechain has one bundle that it means to be paid; others pending for its slot are copies,
    // left by a reorg of the sidechain, with the same withdrawals in them. Paying two in one block
    // would pay those withdrawals twice out of the treasury.
    TwoBundles setup;
    TestChain& chain{setup.chain};
    const CMutableTransaction pay_first{chain.WithdrawalTx(1, setup.first, 2 * COIN, COIN / 10)};
    CMutableTransaction pay_second{setup.second};
    pay_second.vin.assign(1, CTxIn{COutPoint{pay_first.GetHash(), 0}});
    pay_second.vout[0] = CTxOut{pay_first.vout[0].nValue - 2 * COIN - COIN / 10, EscrowScript(1)};
    BOOST_CHECK(!chain.Connect({}, {pay_first, pay_second}));
    // Paying the first failed the second.
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-unknown");
}

BOOST_AUTO_TEST_CASE(paying_a_bundle_fails_the_others)
{
    // Once a bundle of a sidechain is paid, its other pending bundles fail: they can never be paid
    // after it, in a later block either.
    TwoBundles setup;
    TestChain& chain{setup.chain};
    BOOST_REQUIRE(chain.Connect({}, {chain.WithdrawalTx(1, setup.first, 2 * COIN, COIN / 10)}));
    BOOST_CHECK(chain.scdb.WasPaid(1, setup.first.GetHash().ToUint256()) == std::optional<bool>{true});
    BOOST_CHECK(chain.scdb.WasPaid(1, setup.second.GetHash().ToUint256()) == std::optional<bool>{false});
    BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.empty());
    BOOST_CHECK(!chain.Connect({}, {chain.WithdrawalTx(1, setup.second, 2 * COIN, COIN / 10)}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-withdrawal-unknown");
    // Nor can it be proposed again.
    BOOST_CHECK(!chain.Connect({BundleScript(1, setup.second.GetHash().ToUint256())}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bundle-closed");
}

BOOST_AUTO_TEST_CASE(block_without_coinbase)
{
    TestChain chain;
    CBlock empty;
    empty.hashPrevBlock = chain.tip;
    BlockUndo undo;
    std::string reason;
    BOOST_CHECK(!chain.scdb.ConnectBlock(empty, 1, chain.params, undo, nullptr, reason));
    BOOST_CHECK_EQUAL(reason, "bad-dc-no-coinbase");
}

BOOST_AUTO_TEST_CASE(undo_holds_changes_not_slots)
{
    // A block that only votes changes no slot but its bundles' scores: its undo data is the vote,
    // not a copy of the slot with its description and every bundle.
    TestChain chain;
    Sidechain big{MakeSidechain(1)};
    big.description = std::string(MAX_DESCRIPTION_SIZE, 'x');
    chain.Activate(big);
    for (uint8_t i{1}; i <= 3; ++i) BOOST_REQUIRE(chain.Connect({BundleScript(1, uint256{i})}));
    const CBlock block{chain.MakeBlock({Votes({1})})};
    SidechainDB after{chain.scdb};
    BlockUndo undo;
    std::string reason;
    BOOST_REQUIRE(after.ConnectBlock(block, chain.height + 1, chain.params, undo, nullptr, reason));
    BOOST_CHECK(undo.slots.empty());
    BOOST_REQUIRE_EQUAL(undo.bundle_changes.size(), 1U);
    BOOST_CHECK(undo.bundle_changes[0].type == BlockUndo::BundleChange::Type::VOTE);
    DataStream stream{};
    stream << undo;
    BOOST_CHECK_LT(stream.size(), 200U);
    // And it takes the block back exactly (Connect checks every block that way).
    BOOST_REQUIRE(chain.Connect({Votes({1})}));
    BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
    BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
    // Scores at zero stay there, and a downvote leaves the undo data that says which.
    BOOST_REQUIRE(chain.Connect({Votes({2})}));
    // Replacing the sidechain saves the one it replaces, and the bundles it fails.
    chain.Activate(MakeSidechain(1, "replacement"));
    BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.empty());
}

BOOST_AUTO_TEST_CASE(unvoted_bundles_expire)
{
    // A bundle that upvote_expiry_blocks blocks in a row did not upvote fails.
    {
        TestChain chain;
        chain.params.withdrawal_period = 100;
        chain.params.upvote_expiry_blocks = 4;
        chain.Activate(MakeSidechain(1));
        const uint256 junk{0x99};
        BOOST_REQUIRE(chain.Connect({BundleScript(1, junk)}));
        const int proposed{chain.height};
        BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].last_upvote, proposed);
        // Four blocks without an upvote; the fifth finds it expired before its own vote counts.
        for (int i{0}; i < 4; ++i) {
            BOOST_REQUIRE(chain.Connect());
            BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
            BOOST_CHECK(!SidechainDB::UpvoteExpired(chain.scdb.GetSlot(1)->bundles[0], chain.height, chain.params));
        }
        BOOST_CHECK(SidechainDB::UpvoteExpired(chain.scdb.GetSlot(1)->bundles[0], chain.height + 1, chain.params));
        BOOST_REQUIRE(chain.Connect({Votes({0})}));
        BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.empty());
        BOOST_CHECK(chain.scdb.WasPaid(1, junk) == std::optional<bool>{false});
    }

    // An upvote starts the count again; a bundle upvoted at least every upvote_expiry_blocks stays,
    // and one upvoted every block, as the miners that vouch for it do, reaches the score.
    TestChain chain;
    chain.params.withdrawal_period = 100;
    chain.params.withdrawal_min_score = 10;
    chain.params.upvote_expiry_blocks = 4;
    chain.Activate(MakeSidechain(1));
    const uint256 kept{0x98};
    BOOST_REQUIRE(chain.Connect({BundleScript(1, kept)}));
    for (int round{0}; round < 5; ++round) {
        for (int i{0}; i < 3; ++i) BOOST_REQUIRE(chain.Connect());
        BOOST_REQUIRE(chain.Connect({Votes({0})}));
        BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
        BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].last_upvote, chain.height);
    }
    // Downvotes do not count as upvotes.
    for (int i{0}; i < 4; ++i) BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
    BOOST_REQUIRE(chain.Connect());
    BOOST_CHECK(chain.scdb.GetSlot(1)->bundles.empty());

    const uint256 honest{0x97};
    BOOST_REQUIRE(chain.Connect({BundleScript(1, honest)}));
    while (chain.scdb.GetSlot(1)->bundles[0].score < 10U) BOOST_REQUIRE(chain.Connect({Votes({0})}));
    BOOST_CHECK(chain.scdb.GetSlot(1)->bundles[0].hash == honest);
    // Whatever form the upvote takes: REPEAT_PREVIOUS and LEADING_BY_50 upvote too.
    for (int i{0}; i < 6; ++i) BOOST_REQUIRE(chain.Connect({VoteScript({VoteForm::REPEAT_PREVIOUS, {}})}));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
    BOOST_CHECK_EQUAL(chain.scdb.GetSlot(1)->bundles[0].last_upvote, chain.height);
}

BOOST_AUTO_TEST_CASE(miner_keeps_its_bundle_upvoted)
{
    // The bundle a sidechain node vouches for is proposed by this node's blocks and upvoted by every
    // one after: however short upvote_expiry_blocks, it stays until it is paid.
    TestChain chain;
    chain.params.withdrawal_period = 100;
    chain.params.withdrawal_min_score = 10;
    chain.params.upvote_expiry_blocks = 1;
    chain.Activate(MakeSidechain(1));
    BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(1, 10 * COIN)}));
    MinerState miner;
    std::string error;
    const auto hash{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000), error)};
    BOOST_REQUIRE(hash);
    bool paid{false};
    for (int i{0}; i < 20 && !paid; ++i) {
        const BlockAdditions additions{miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {})};
        std::vector<CMutableTransaction> txs;
        for (const CTransactionRef& tx : additions.withdrawals) txs.emplace_back(*tx);
        BOOST_REQUIRE_MESSAGE(chain.Connect(Scripts(additions), txs), chain.reject_reason);
        paid = chain.scdb.WasPaid(1, *hash) == std::optional<bool>{true};
        if (!paid) BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
    }
    BOOST_CHECK(paid);
}

BOOST_AUTO_TEST_CASE(failed_bundles_are_forgotten)
{
    // A failed bundle is forgotten withdrawal_period blocks after it failed; a paid one never is.
    {
        TwoBundles setup;
        TestChain& chain{setup.chain};
        const uint256 first{setup.first.GetHash().ToUint256()}, second{setup.second.GetHash().ToUint256()};
        BOOST_REQUIRE(chain.Connect({}, {chain.WithdrawalTx(1, setup.first, 2 * COIN, COIN / 10)}));
        const int failed{chain.height};
        BOOST_REQUIRE(chain.scdb.WasPaid(1, second) == std::optional<bool>{false});
        while (chain.height < failed + chain.params.withdrawal_period - 1) {
            BOOST_REQUIRE(chain.Connect());
            BOOST_REQUIRE(chain.scdb.WasPaid(1, second) == std::optional<bool>{false});
        }
        // Still remembered: it cannot be proposed again.
        BOOST_CHECK(!chain.Connect({BundleScript(1, second)}));
        BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bundle-closed");
        const size_t closed{chain.scdb.ClosedCount()};
        BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK(!chain.scdb.WasPaid(1, second).has_value());
        BOOST_CHECK_EQUAL(chain.scdb.ClosedCount(), closed - 1);
        BOOST_CHECK(chain.scdb.WasPaid(1, first) == std::optional<bool>{true});
        // Forgotten, it can be proposed again (and would need the votes all over).
        BOOST_CHECK(chain.Connect({BundleScript(1, second)}));
        // A paid bundle never can.
        BOOST_CHECK(!chain.Connect({BundleScript(1, first)}));
        BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-bundle-closed");
    }
}

BOOST_AUTO_TEST_CASE(miner_never_reproposes_a_closed_bundle)
{
    // A bundle the chain closed once is not proposed or upvoted again by the miner, even after the
    // sidechain database forgot that it failed and it looks new (a node that did not build blocks
    // meanwhile, so whose miner never saw it closed, included).
    TestChain chain;
    chain.params.upvote_expiry_blocks = 3;
    chain.Activate(MakeSidechain(1));
    BOOST_REQUIRE(chain.Connect({}, {chain.DepositTx(1, 10 * COIN)}));
    MinerState miner;
    std::string error;
    const auto hash{miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000), error)};
    BOOST_REQUIRE(hash);
    miner.Prune(chain.scdb, chain.height + 1);
    BOOST_REQUIRE(chain.Connect(Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {}))));
    BOOST_REQUIRE_EQUAL(chain.scdb.GetSlot(1)->bundles.size(), 1U);
    // Other miners downvote it until it fails; this node builds no block meanwhile.
    std::optional<int> closed_at;
    while (!closed_at) {
        BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
        if (chain.scdb.WasPaid(1, *hash) == std::optional<bool>{false}) closed_at = chain.height;
    }
    while (chain.scdb.IsClosed(1, *hash)) BOOST_REQUIRE(chain.Connect());
    // Forgotten. What the chain's history says of it, as the index of closures would.
    const MinerState::ClosureLookup history{[&](SidechainId slot, const uint256& h) -> std::optional<int> {
        if (slot == 1 && h == *hash) return closed_at;
        return std::nullopt;
    }};
    // Without the history it looks new, and would be proposed again.
    {
        MinerState blind_miner;
        BOOST_REQUIRE(blind_miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000), error));
        blind_miner.Prune(chain.scdb, chain.height + 1);
        BOOST_CHECK(Contains(Scripts(blind_miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {})), BundleScript(1, *hash)));
    }
    miner.Prune(chain.scdb, chain.height + 1, history);
    BOOST_CHECK(!Contains(Scripts(miner.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {})), BundleScript(1, *hash)));
    // Closed long enough ago: dropped.
    BOOST_CHECK(!miner.GetBundle(1, *hash));

    // Proposed again by someone else, it is not upvoted, by a miner that was just handed it either.
    BOOST_REQUIRE(chain.Connect({BundleScript(1, *hash)}));
    MinerState handed_again;
    BOOST_REQUIRE(handed_again.AddBundle(1, TestChain::BlindBundle(COIN, 1000), error));
    BOOST_CHECK(handed_again.ResolveVote(1, chain.scdb.GetSlot(1)->bundles).type == Vote::Type::UPVOTE);
    handed_again.Prune(chain.scdb, chain.height + 1, history);
    BOOST_CHECK(handed_again.ResolveVote(1, chain.scdb.GetSlot(1)->bundles).type != Vote::Type::UPVOTE);
    BOOST_CHECK(!Contains(Scripts(handed_again.CreateBlockAdditions(chain.scdb, chain.params, chain.tip, {})), Votes({0})));
}

BOOST_AUTO_TEST_CASE(miner_drops_bundles_never_proposed)
{
    // A handed bundle no block proposed goes after UNPROPOSED_EXPIRY blocks, unless it is the one the
    // sidechain node vouches for; when it was handed is kept across restarts.
    TestChain chain;
    chain.Activate(MakeSidechain(1));
    const fs::path path{m_path_root / "drivechain_miner.dat"};
    std::string error;
    std::optional<uint256> old_bundle, new_bundle;
    {
        MinerState miner;
        BOOST_REQUIRE(miner.Load(path));
        old_bundle = miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 1), error);
        new_bundle = miner.AddBundle(1, TestChain::BlindBundle(COIN, 1000, 2), error);
        BOOST_REQUIRE(old_bundle && new_bundle);
        miner.Prune(chain.scdb, 100);
    }
    MinerState miner;
    BOOST_REQUIRE(miner.Load(path));
    miner.Prune(chain.scdb, 100 + MinerState::UNPROPOSED_EXPIRY - 1);
    BOOST_CHECK(miner.GetBundle(1, *old_bundle));
    miner.Prune(chain.scdb, 100 + MinerState::UNPROPOSED_EXPIRY);
    BOOST_CHECK(!miner.GetBundle(1, *old_bundle));
    BOOST_CHECK(miner.GetBundle(1, *new_bundle));
}

BOOST_AUTO_TEST_CASE(unvoted_failed_bundles_are_forgotten_sooner)
{
    // A failed bundle no block upvoted is forgotten unvoted_forget_blocks after it failed; one that
    // was upvoted, a withdrawal period after.
    {
        TestChain chain;
        chain.params.upvote_expiry_blocks = 3;
        chain.params.unvoted_forget_blocks = 5;
        chain.Activate(MakeSidechain(1));
        const uint256 unvoted{0xd1}, upvoted{0xd2};
        BOOST_REQUIRE(chain.Connect({BundleScript(1, unvoted)}));
        BOOST_REQUIRE(chain.Connect({BundleScript(1, upvoted)}));
        BOOST_REQUIRE(chain.Connect({Votes({1})}));
        // Both fail: three blocks without an upvote (the downvotes take them to 0 meanwhile).
        int failed{0}, failed_upvoted{0};
        while (chain.scdb.GetSlot(1)->bundles.size() > 0) {
            BOOST_REQUIRE(chain.Connect({Votes({VOTE_DOWNVOTE})}));
            if (!failed && chain.scdb.IsClosed(1, unvoted)) failed = chain.height;
            if (!failed_upvoted && chain.scdb.IsClosed(1, upvoted)) failed_upvoted = chain.height;
        }
        BOOST_REQUIRE(chain.scdb.GetClosed(1, unvoted) && !chain.scdb.GetClosed(1, unvoted)->upvoted);
        BOOST_REQUIRE(chain.scdb.GetClosed(1, upvoted) && chain.scdb.GetClosed(1, upvoted)->upvoted);
        while (chain.height < failed + chain.params.unvoted_forget_blocks - 1) BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK(chain.scdb.IsClosed(1, unvoted));
        BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK(!chain.scdb.IsClosed(1, unvoted));
        BOOST_CHECK(chain.scdb.IsClosed(1, upvoted));
        // The snapshot keeps them apart: read back, it forgets the same.
        DataStream stream{};
        stream << chain.scdb;
        SidechainDB read;
        stream >> read;
        BOOST_CHECK(read == chain.scdb);
        chain.scdb = read;
        while (chain.height < failed_upvoted + chain.params.withdrawal_period - 1) BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK(chain.scdb.IsClosed(1, upvoted));
        BOOST_REQUIRE(chain.Connect());
        BOOST_CHECK(!chain.scdb.IsClosed(1, upvoted));
        BOOST_CHECK(!chain.scdb.IsClosed(1, unvoted));
    }
}

BOOST_AUTO_TEST_CASE(state_snapshot_and_index)
{
    // The failed bundles by height are rebuilt when a snapshot is read: what is forgotten next is the same.
    TwoBundles setup;
    TestChain& chain{setup.chain};
    BOOST_REQUIRE(chain.Connect({}, {chain.WithdrawalTx(1, setup.first, 2 * COIN, COIN / 10)}));
    DataStream stream{};
    stream << chain.scdb;
    SidechainDB read;
    stream >> read;
    BOOST_CHECK(read == chain.scdb);
    chain.scdb = read;
    chain.ConnectEmpty(chain.params.withdrawal_period);
    BOOST_CHECK(!chain.scdb.WasPaid(1, setup.second.GetHash().ToUint256()));
}

BOOST_AUTO_TEST_CASE(database_records)
{
    // The drivechain database on its own: what each block keeps, the snapshot and its version, the
    // deposit index.
    Database db{DBParams{.path = m_path_root / "drivechain_db", .cache_bytes = 1 << 20, .memory_only = true}, ParamsFingerprint(TestParams())};
    // A new database is in the current format; one with data and no version is an older one.
    BOOST_CHECK(db.IsCurrentFormat());

    TestChain chain;
    chain.Activate(MakeSidechain(1));
    std::vector<std::pair<uint256, std::vector<Deposit>>> blocks;
    std::vector<uint256> txids;
    for (int i{1}; i <= 5; ++i) {
        const CMutableTransaction deposit{chain.DepositTx(1, i * COIN)};
        const CBlock block{chain.MakeBlock({}, {deposit})};
        BlockUndo undo;
        std::vector<Deposit> deposits;
        std::string reason;
        BOOST_REQUIRE(chain.scdb.ConnectBlock(block, chain.height + 1, chain.params, undo, &deposits, reason));
        for (Deposit& d : deposits) {
            d.tx = block.vtx[1];
            d.tx_index = 1;
            d.block_hash = block.GetHash();
        }
        ++chain.height;
        chain.tip = block.GetHash();
        // The last two keep their undo data; the others only what sidechains follow.
        BOOST_REQUIRE(db.WriteBlock(block.GetHash(), chain.height, undo, deposits, chain.scdb, /*keep_undo=*/i > 3));
        blocks.emplace_back(block.GetHash(), deposits);
        txids.push_back(block.vtx[1]->GetHash().ToUint256());
    }
    BlockUndo undo;
    BlockEvents events;
    BOOST_CHECK(!db.HasBlockUndo(blocks[0].first));
    BOOST_CHECK(!db.ReadBlockUndo(blocks[0].first, undo));
    BOOST_CHECK(db.ReadBlockEvents(blocks[0].first, events));
    BOOST_CHECK(db.HasBlockUndo(blocks[4].first));
    db.EraseBlockUndo(blocks[4].first);
    BOOST_CHECK(!db.HasBlockUndo(blocks[4].first));
    BOOST_CHECK(db.ReadBlockEvents(blocks[4].first, events));
    BOOST_CHECK(events.pending.empty());

    // The bundles pending after a block go with its events, with their scores after it.
    {
        const uint256 bundle{0xb1};
        const CBlock block{chain.MakeBlock({BundleScript(1, bundle)})};
        BlockUndo block_undo;
        std::vector<Deposit> none;
        std::string reason;
        BOOST_REQUIRE(chain.scdb.ConnectBlock(block, chain.height + 1, chain.params, block_undo, &none, reason));
        ++chain.height;
        chain.tip = block.GetHash();
        BOOST_REQUIRE(db.WriteBlock(block.GetHash(), chain.height, block_undo, none, chain.scdb));
        BlockEvents read_events;
        BOOST_REQUIRE(db.ReadBlockEvents(block.GetHash(), read_events));
        BOOST_CHECK_EQUAL(read_events.proposed.size(), 1U);
        BOOST_REQUIRE_EQUAL(read_events.pending.size(), 1U);
        BOOST_CHECK_EQUAL(read_events.pending[0].first, 1U);
        BOOST_REQUIRE_EQUAL(read_events.pending[0].second.size(), 1U);
        BOOST_CHECK(read_events.pending[0].second[0].first == bundle);
        BOOST_CHECK_EQUAL(read_events.pending[0].second[0].second, NEW_BUNDLE_SCORE);
    }

    // The blocks that closed a bundle, for good; only those the caller takes for the active chain count.
    {
        BlockUndo closing;
        closing.closed.push_back({1, uint256{0xc1}, false});
        const uint256 a{0xa1}, b{0xa2};
        BOOST_REQUIRE(db.WriteBlock(a, 50, closing, {}, chain.scdb));
        BOOST_REQUIRE(db.WriteBlock(b, 60, closing, {}, chain.scdb));
        BOOST_REQUIRE(db.WriteBlock(b, 60, closing, {}, chain.scdb));
        const auto all{[](const uint256&) { return true; }};
        BOOST_CHECK_EQUAL(db.FindClosure(1, uint256{0xc1}, all)->height, 60);
        BOOST_CHECK(db.FindClosure(1, uint256{0xc1}, all)->block_hash == b);
        BOOST_CHECK(!db.FindClosure(1, uint256{0xc1}, all)->paid);
        BOOST_CHECK_EQUAL(db.FindClosure(1, uint256{0xc1}, [&](const uint256& h) { return h == a; })->height, 50);
        BOOST_CHECK(!db.FindClosure(1, uint256{0xc1}, [](const uint256&) { return false; }));
        BOOST_CHECK(!db.FindClosure(2, uint256{0xc1}, all));
    }

    const auto any{[](const uint256&) { return true; }};
    // Deposits after one, found by its txid, a few at a time.
    const auto all{db.ListDeposits(1, std::nullopt, 0, any)};
    BOOST_REQUIRE(all);
    BOOST_CHECK_EQUAL(all->size(), 5U);
    const auto after{db.ListDeposits(1, txids[1], 2, any)};
    BOOST_REQUIRE(after);
    BOOST_REQUIRE_EQUAL(after->size(), 2U);
    BOOST_CHECK((*after)[0].tx->GetHash().ToUint256() == txids[2]);
    BOOST_CHECK((*after)[1].tx->GetHash().ToUint256() == txids[3]);
    BOOST_CHECK(db.ListDeposits(1, txids[4], 0, any)->empty());
    BOOST_CHECK(!db.ListDeposits(1, uint256{0x77}, 0, any));
    BOOST_CHECK(!db.ListDeposits(2, txids[0], 0, any));
    // Records of blocks that are not in the active chain are skipped, and not counted.
    {
        const auto without_second{[&](const uint256& h) { return h != blocks[1].first; }};
        const auto page{db.ListDeposits(1, txids[0], 2, without_second)};
        BOOST_REQUIRE(page);
        BOOST_REQUIRE_EQUAL(page->size(), 2U);
        BOOST_CHECK((*page)[0].tx->GetHash().ToUint256() == txids[2]);
        BOOST_CHECK((*page)[1].tx->GetHash().ToUint256() == txids[3]);
        // Nor taken as the place to go on from.
        BOOST_CHECK(!db.ListDeposits(1, txids[1], 0, without_second));
    }
    // Erasing the records of a block takes its deposits, and their txids, with it.
    BOOST_CHECK(db.EraseBlockDeposits(blocks[4].first));
    BOOST_CHECK(!db.ListDeposits(1, txids[4], 0, any));
    BOOST_CHECK_EQUAL(db.ListDeposits(1, std::nullopt, 0, any)->size(), 4U);
    // A block of another branch at the same height, with the same transaction at the same position
    // (a crash or a reorg back can leave both): each keeps its own records, and readers go by the
    // active chain, whichever was written last.
    {
        const uint256 stale{0x5e};
        const int height{chain.height - 3}; // the height of blocks[2] (a block with a bundle came after blocks[4])
        Deposit copy{blocks[2].second.at(0)};
        copy.block_hash = stale;
        BOOST_REQUIRE(db.WriteBlock(stale, height, BlockUndo{}, {copy}, chain.scdb));
        BOOST_CHECK_EQUAL(db.ListDeposits(1, std::nullopt, 0, any)->size(), 5U);
        const auto active{[&](const uint256& h) { return h != stale; }};
        const auto page{db.ListDeposits(1, txids[2], 0, active)};
        BOOST_REQUIRE(page);
        BOOST_REQUIRE_EQUAL(page->size(), 1U);
        BOOST_CHECK((*page)[0].tx->GetHash().ToUint256() == txids[3]);
        const auto listed{db.ListDeposits(1, txids[1], 0, active)};
        BOOST_REQUIRE(listed);
        BOOST_REQUIRE_EQUAL(listed->size(), 2U);
        BOOST_CHECK((*listed)[0].block_hash == blocks[2].first);
        // The other way around: the stale branch is the active one.
        const auto other{[&](const uint256& h) { return h != blocks[2].first; }};
        const auto from_stale{db.ListDeposits(1, txids[1], 0, other)};
        BOOST_REQUIRE(from_stale);
        BOOST_REQUIRE_EQUAL(from_stale->size(), 2U);
        BOOST_CHECK((*from_stale)[0].block_hash == stale);
        // By block.
        const auto in_block{db.ListBlockDeposits(1, height, blocks[2].first)};
        BOOST_REQUIRE_EQUAL(in_block.size(), 1U);
        BOOST_CHECK(in_block[0].block_hash == blocks[2].first);
        BOOST_REQUIRE_EQUAL(db.ListBlockDeposits(1, height, stale).size(), 1U);
        BOOST_CHECK(db.ListBlockDeposits(1, height, uint256{0x99}).empty());
        // Erasing the stale block's records leaves the active one's.
        BOOST_CHECK(db.EraseBlockDeposits(stale));
        BOOST_CHECK_EQUAL(db.ListBlockDeposits(1, height, blocks[2].first).size(), 1U);
        BOOST_CHECK_EQUAL(db.ListDeposits(1, txids[1], 0, any)->size(), 2U);
    }

    // The snapshot: read back as written; a missing or unreadable one leaves nothing behind.
    BOOST_REQUIRE(db.WriteState("", chain.scdb));
    SidechainDB read;
    BOOST_CHECK(db.ReadState("", read));
    BOOST_CHECK(read == chain.scdb);
    BOOST_CHECK(!db.ReadState("other", read));
    BOOST_CHECK(read == SidechainDB{});

    db.Wipe();
    BOOST_CHECK(!db.IsCurrentFormat());
    db.WriteFormatVersion();
    BOOST_CHECK(db.IsCurrentFormat());
    BOOST_CHECK(!db.ReadState("", read));
    BOOST_CHECK(!db.ReadBlockEvents(blocks[0].first, events));
    BOOST_CHECK(!db.ListDeposits(1, txids[0], 0, any));
}

BOOST_AUTO_TEST_CASE(database_params_fingerprint)
{
    // What was derived under other drivechain parameters is not used: not the format marker, not the snapshot.
    const Consensus::DrivechainParams params{TestParams()};
    const uint256 fingerprint{ParamsFingerprint(params)};
    const auto changed{[&](auto change) {
        Consensus::DrivechainParams other{params};
        change(other);
        return ParamsFingerprint(other);
    }};
    BOOST_CHECK(changed([](auto& p) { ++p.upvote_expiry_blocks; }) != fingerprint);
    BOOST_CHECK(changed([](auto& p) { ++p.unvoted_forget_blocks; }) != fingerprint);
    BOOST_CHECK(changed([](auto& p) { ++p.idle_expiry_blocks; }) != fingerprint);
    BOOST_CHECK(changed([](auto& p) { ++p.withdrawal_period; }) != fingerprint);
    BOOST_CHECK(changed([](auto& p) { --p.max_sidechains; }) != fingerprint);
    BOOST_CHECK(changed([](auto&) {}) == fingerprint);
    const uint256 other{changed([](auto& p) { ++p.upvote_expiry_blocks; })};

    // On a sidechain, the parameters of its own rules count too; on any other chain they do not.
    Consensus::SidechainParams sidechain;
    BOOST_CHECK(ParamsFingerprint(params, sidechain) == fingerprint);
    sidechain.enabled = true;
    const uint256 side_fingerprint{ParamsFingerprint(params, sidechain)};
    BOOST_CHECK(side_fingerprint != fingerprint);
    const auto side_changed{[&](auto change) {
        Consensus::SidechainParams other_side{sidechain};
        change(other_side);
        return ParamsFingerprint(params, other_side);
    }};
    BOOST_CHECK(side_changed([](auto& p) { ++p.bundle_retry_delay; }) != side_fingerprint);
    BOOST_CHECK(side_changed([](auto& p) { ++p.pending_min_score; }) != side_fingerprint);
    BOOST_CHECK(side_changed([](auto& p) { ++p.unproposed_expiry_blocks; }) != side_fingerprint);
    BOOST_CHECK(side_changed([](auto& p) { ++p.main_activation_height; }) != side_fingerprint);
    BOOST_CHECK(side_changed([](auto&) {}) == side_fingerprint);
    {
        Consensus::DrivechainParams other_params{params};
        ++other_params.upvote_expiry_blocks;
        BOOST_CHECK(ParamsFingerprint(other_params, sidechain) != side_fingerprint);
    }

    const fs::path path{m_path_root / "drivechain_fingerprint"};
    TestChain chain;
    const sidechain::StoreBytes side_key{'x', 1};
    {
        Database db{DBParams{.path = path, .cache_bytes = 1 << 20}, fingerprint};
        BOOST_CHECK(db.CheckFormat() == Database::Format::CURRENT);
        // With an entry in the store of the sidechain state, written with the snapshot.
        const auto side_db{db.SideStore("")};
        const std::map<sidechain::StoreBytes, std::optional<sidechain::StoreBytes>> changes{{side_key, sidechain::StoreBytes{'v'}}};
        BOOST_REQUIRE(db.WriteState("", chain.scdb, side_db.get(), &changes));
        SidechainDB read;
        BOOST_CHECK(db.ReadState("", read));
        BOOST_CHECK(side_db->Get(side_key).has_value());
    }
    {
        Database db{DBParams{.path = path, .cache_bytes = 1 << 20}, other};
        BOOST_CHECK(db.CheckFormat() == Database::Format::OTHER_PARAMS);
        SidechainDB read;
        BOOST_CHECK(!db.ReadState("", read));
        // The rebuild starts the store over with the rest.
        const auto side_db{db.SideStore("")};
        db.Wipe();
        side_db->Reset();
        BOOST_CHECK(!side_db->Get(side_key).has_value());
        db.WriteFormatVersion();
        BOOST_CHECK(db.IsCurrentFormat());
    }
}

BOOST_FIXTURE_TEST_CASE(undo_erased_below_the_flushed_block, TestChain100Setup)
{
    // The drivechain undo data of a block goes once it is DRIVECHAIN_UNDO_DEPTH below the last
    // flushed block, not below the tip: a node that stops uncleanly restarts from the flushed block,
    // and a reorg from there must find the undo data of every block within reach.
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    const auto check{[&] {
        LOCK(::cs_main);
        const int flushed{chainstate.GetLastFlushedBlock()->nHeight};
        for (int height{1}; height <= chainstate.m_chain.Height(); ++height) {
            const bool kept{m_node.chainman->m_blockman.m_drivechain_db->HasBlockUndo(chainstate.m_chain[height]->GetBlockHash())};
            BOOST_CHECK_MESSAGE(kept == (height > flushed - DRIVECHAIN_UNDO_DEPTH), strprintf("height %d, flushed %d", height, flushed));
        }
        return flushed;
    }};
    const auto flush{[&] {
        LOCK(::cs_main);
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(state, FlushStateMode::FORCE_FLUSH));
    }};
    flush();
    const int flushed{check()};
    mineBlocks(DRIVECHAIN_UNDO_DEPTH + 20);
    // Unless a periodic flush came meanwhile, nothing was erased: the tip is far ahead of what is on disk.
    if (check() == flushed) {
        LOCK(::cs_main);
        BOOST_CHECK(m_node.chainman->m_blockman.m_drivechain_db->HasBlockUndo(chainstate.m_chain[chainstate.m_chain.Height() - DRIVECHAIN_UNDO_DEPTH]->GetBlockHash()));
    }
    flush();
    BOOST_CHECK_EQUAL(check(), WITH_LOCK(::cs_main, return chainstate.m_chain.Height()));
}

BOOST_AUTO_TEST_SUITE_END()
