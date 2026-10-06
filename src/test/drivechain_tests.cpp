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
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <optional>
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
    BOOST_CHECK(!chain.Connect({}, {chain.DepositTx(2, 9 * COIN, std::string(MAX_DEPOSIT_DESTINATION_SIZE + 1, 'x'))}));

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

    // There is a limit to the number of pending bundles.
    BOOST_CHECK(!chain.Connect({BundleScript(0, uint256{0xe1})}));
    BOOST_CHECK_EQUAL(chain.reject_reason, "bad-dc-too-many-bundles");
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
    BOOST_CHECK(!chain.Connect({BmmAcceptScript(2, h1)}, {request_tx(1, h1, chain.tip)}));
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

BOOST_AUTO_TEST_CASE(payout_queue)
{
    // A block pays at most MAX_PAYOUTS_PER_BLOCK outputs; the rest wait, in order, and undo restores them.
    sidechain::State state;
    std::vector<CTxOut> owed;
    for (size_t i{0}; i < sidechain::MAX_PAYOUTS_PER_BLOCK + 500; ++i) owed.emplace_back(static_cast<CAmount>(i + 1), CScript() << OP_TRUE);
    sidechain::StateUndo first;
    const auto paid1{state.TakePayouts(owed, {}, first)};
    BOOST_REQUIRE_EQUAL(paid1.size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK(paid1.front() == owed.front() && paid1.back() == owed[sidechain::MAX_PAYOUTS_PER_BLOCK - 1]);
    BOOST_CHECK_EQUAL(state.Queue().size(), 500U);
    const sidechain::State after_first{state};

    sidechain::StateUndo second;
    const CTxOut extra{7 * COIN, CScript() << OP_TRUE << OP_TRUE};
    const auto paid2{state.TakePayouts({}, {extra}, second)};
    BOOST_REQUIRE_EQUAL(paid2.size(), 501U);
    BOOST_CHECK(paid2.front() == owed[sidechain::MAX_PAYOUTS_PER_BLOCK] && paid2.back() == extra);
    BOOST_CHECK(state.Queue().empty());

    state.DisconnectBlock(second);
    BOOST_CHECK(state == after_first);
    state.DisconnectBlock(first);
    BOOST_CHECK(state.Queue().empty());

    // Payouts of transactions, however many, do not hold back deposits: those go first.
    std::vector<CTxOut> spam(sidechain::MAX_PAYOUTS_PER_BLOCK + 10, CTxOut{1, CScript() << OP_TRUE});
    sidechain::StateUndo third, fourth;
    BOOST_CHECK_EQUAL(state.TakePayouts({}, spam, third).size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    const CTxOut deposit{5 * COIN, CScript() << OP_TRUE << OP_TRUE};
    const auto paid4{state.TakePayouts({deposit}, {}, fourth)};
    BOOST_REQUIRE_EQUAL(paid4.size(), 11U);
    BOOST_CHECK(paid4.front() == deposit);
    BOOST_CHECK(state.TxQueue().empty());
    state.DisconnectBlock(fourth);
    BOOST_CHECK_EQUAL(state.TxQueue().size(), 10U);
}

BOOST_AUTO_TEST_CASE(bundle_nonce)
{
    // A bundle carries the hash of the block before the one that commits to it: its hash cannot be known in advance.
    Consensus::SidechainParams params;
    sidechain::State state;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), GetScriptForDestination(WitnessV0KeyHash{uint160::FromHex("0101010101010101010101010101010101010101").value()})));
    sidechain::StateUndo undo;
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 1, params, undo, payouts, reason), reason);
    const auto a{state.NextBundle(10, uint256{0xa}, params)};
    const auto b{state.NextBundle(10, uint256{0xb}, params)};
    BOOST_REQUIRE(a && b);
    BOOST_CHECK(a->GetHash() != b->GetHash());
    const uint256 prev{0xa};
    BOOST_CHECK(a->vout.back().scriptPubKey == (CScript() << OP_RETURN << std::vector<unsigned char>(prev.begin(), prev.end())));
    BOOST_CHECK(IsBlindWithdrawal(CTransaction{*a}));
    // The pending bundle remembers it, so that the bundle can be built again.
    BOOST_REQUIRE_MESSAGE(state.StartBundle(a->GetHash().ToUint256(), 10, uint256{0xa}, params, undo, reason), reason);
    BOOST_CHECK(state.BundleTx()->GetHash() == a->GetHash());
}

BOOST_AUTO_TEST_CASE(paid_on_another_branch)
{
    // A bundle committed on another branch of this chain, which this branch never had, is paid out on
    // the mainchain: the withdrawals it paid are paid on this branch too, and cannot be paid again.
    Consensus::SidechainParams params;
    sidechain::State state;
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{uint160::FromHex("0101010101010101010101010101010101010101").value()})};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), pay));
    tx.vout.emplace_back(2 * COIN, sidechain::WithdrawalScript(1000, uint160::FromHex("0101010101010101010101010101010101010101").value(), pay));
    sidechain::StateUndo undo;
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 1, params, undo, payouts, reason), reason);
    BOOST_REQUIRE_EQUAL(state.Withdrawals().size(), 2U);

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

    const sidechain::State before{state};
    sidechain::StateUndo main_undo;
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(1, mainchain, 2, params, main_undo, payouts, reason), reason);
    // The one withdrawal that pays that output with that amount is gone; the other stays.
    BOOST_REQUIRE_EQUAL(state.Withdrawals().size(), 1U);
    BOOST_CHECK_EQUAL(state.Withdrawals().begin()->second.amount, 2 * COIN - 1000);
    state.DisconnectBlock(main_undo);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(paid_on_another_branch_oldest_first)
{
    // Of several withdrawals that pay the same output with the same amount, a payout of a bundle of
    // another branch takes the oldest, whatever their outpoints.
    Consensus::SidechainParams params;
    sidechain::State state;
    const uint160 key{uint160::FromHex("0101010101010101010101010101010101010101").value()};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key})};
    std::vector<CTxOut> payouts;
    std::string reason;
    const auto withdraw{[&](uint8_t salt, int height) {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{salt}), 0};
        tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, key, pay));
        sidechain::StateUndo undo;
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, height, params, undo, payouts, reason), reason);
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
    sidechain::StateUndo undo;
    BOOST_REQUIRE_MESSAGE(state.ApplyMainEvents(1, mainchain, 3, params, undo, payouts, reason), reason);
    BOOST_REQUIRE_EQUAL(state.Withdrawals().size(), 1U);
    BOOST_CHECK(state.Withdrawals().contains(newer));
    BOOST_CHECK(!state.Withdrawals().contains(older));
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

BOOST_AUTO_TEST_SUITE_END()
