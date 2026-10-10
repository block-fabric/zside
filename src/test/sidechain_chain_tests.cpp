// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The sidechain rules in a node: a chainstate of this chain as a sidechain, with a record of the
// mainchain the test writes itself. Validation, block assembly and the mempool, where they act on the
// sidechain state.

#include <addresstype.h>
#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <common/signmessage.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <chainparams.h>
#include <init.h>
#include <drivechain/db.h>
#include <interfaces/mining.h>
#include <key.h>
#include <key_io.h>
#include <node/chainstate.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

/** The verdict validation gave on the blocks it checked, by hash. */
struct BlockCatcher final : public CValidationInterface {
    std::map<uint256, BlockValidationState> states;
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        states[block->GetHash()] = state;
    }
};

//! The mainchain fee of the withdrawals of the tests.
constexpr CAmount MAIN_FEE{1000};
//! The fee of the transactions of the tests.
constexpr CAmount TX_FEE{10000};

/** A chain that is a sidechain in slot 3, and the record of its mainchain. */
struct SidechainChainSetup : public TestingSetup {
    CKey key;
    CScript script;
    std::shared_ptr<BlockCatcher> catcher{std::make_shared<BlockCatcher>()};

    SidechainChainSetup() : TestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=3"}}}
    {
        key.MakeNewKey(/*fCompressed=*/true);
        script = GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()});
        m_node.validation_signals->RegisterSharedValidationInterface(catcher);
        // The first block of the mainchain, which no sidechain acts on.
        Main();
    }
    ~SidechainChainSetup()
    {
        m_node.validation_signals->UnregisterSharedValidationInterface(catcher);
    }

    ChainstateManager& Chainman() { return *Assert(m_node.chainman); }
    Chainstate& Chain() { return Chainman().ActiveChainstate(); }
    sidechain::Mainchain& Record() { return *Assert(Chainman().m_mainchain); }
    const CBlockIndex* Tip() { return WITH_LOCK(::cs_main, return Chain().m_chain.Tip()); }
    const Consensus::SidechainParams& Params() { return Chainman().GetConsensus().sidechain; }

    /** Append a block to the record of the mainchain. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int height{Record().Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x10000 + height)} << 128);
        block.prev_hash = Record().TipHash();
        if (fill) fill(block);
        BOOST_REQUIRE(Record().Append(block));
    }

    /** A deposit to `script` in a new block of the mainchain. */
    void Deposit(CAmount amount)
    {
        Main([&](sidechain::MainBlock& block) {
            sidechain::MainDeposit deposit;
            deposit.destination = EncodeDestination(WitnessV0KeyHash{key.GetPubKey()});
            deposit.amount = amount;
            deposit.txid = uint256{static_cast<uint8_t>(Record().Height())};
            block.deposits.push_back(deposit);
        });
    }

    /** The block the node would build on its tip for the next mainchain block. */
    CBlock Build(bool use_mempool = true)
    {
        auto block_template{interfaces::MakeMining(m_node)->createNewBlock({.use_mempool = use_mempool, .coinbase_output_script = CScript() << OP_TRUE}, /*cooldown=*/false)};
        BOOST_REQUIRE(block_template);
        CBlock block{block_template->getBlock()};
        Finish(block);
        return block;
    }

    /** After a change: the merkle root, and the formality of the proof of work. */
    void Finish(CBlock& block)
    {
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Chainman().GetConsensus())) ++block.nNonce;
    }

    /** Have the next mainchain block commit to the block, and give the block to the node. */
    void Submit(const CBlock& block, bool commit = true)
    {
        if (commit) Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        Chainman().ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
    }

    /** Mine a valid block. */
    CBlock Mine(bool use_mempool = true)
    {
        const CBlock block{Build(use_mempool)};
        Submit(block);
        BOOST_REQUIRE(Tip()->GetBlockHash() == block.GetHash());
        return block;
    }

    /** Why validation refused the block; empty if it did not. */
    std::string Reason(const CBlock& block)
    {
        const auto it{catcher->states.find(block.GetHash())};
        if (it == catcher->states.end() || it->second.IsValid()) return {};
        return it->second.GetRejectReason();
    }

    /** A coin of `amount` for the key, from a deposit; `coin` is set to it. */
    CTxOut Fund(CAmount amount, COutPoint& coin)
    {
        Deposit(amount);
        const CBlock block{Mine()};
        const CTransaction& coinbase{*block.vtx[0]};
        BOOST_REQUIRE(coinbase.vout.size() >= 2);
        BOOST_REQUIRE(coinbase.vout[1].scriptPubKey == script);
        coin = COutPoint{coinbase.GetHash(), 1};
        return coinbase.vout[1];
    }

    /** A transaction spending `prevout` of the key, signed. */
    CMutableTransaction Spend(const COutPoint& prevout, const CTxOut& spent, std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout = std::move(outputs);
        FillableSigningProvider keystore;
        BOOST_REQUIRE(keystore.AddKey(key));
        std::map<COutPoint, Coin> coins{{prevout, Coin{spent, 1, /*fCoinBaseIn=*/false}}};
        std::map<int, bilingual_str> errors;
        BOOST_REQUIRE(SignTransaction(tx, &keystore, coins, SignOptions{.sighash_type = SIGHASH_ALL}, errors));
        return tx;
    }

    MempoolAcceptResult Accept(const CMutableTransaction& tx, bool test_accept = false)
    {
        return WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(tx), test_accept));
    }

    bool InMempool(const CMutableTransaction& tx) { return m_node.mempool->exists(tx.GetHash()); }

    /** A withdrawal of `value` (the mainchain fee included), refundable by the key, with change. */
    CMutableTransaction Withdraw(const COutPoint& coin, const CTxOut& spent, CAmount value)
    {
        return Spend(coin, spent, {CTxOut{value, sidechain::WithdrawalScript(MAIN_FEE, key.GetPubKey().GetID(), script)},
                                   CTxOut{spent.nValue - value - TX_FEE, script}});
    }

    /** A refund request for `withdrawal`, spending the change of `funding` (output 1). */
    CMutableTransaction Refund(const COutPoint& withdrawal, const CMutableTransaction& funding, const CKey* signer = nullptr)
    {
        sidechain::RefundRequest request;
        request.withdrawal = withdrawal;
        BOOST_REQUIRE((signer ? *signer : key).SignCompact(MessageHash(sidechain::RefundMessage(withdrawal)), request.signature));
        const CTxOut& change{funding.vout[1]};
        return Spend(COutPoint{funding.GetHash(), 1}, change, {CTxOut{0, sidechain::RefundScript(request)}, CTxOut{change.nValue - TX_FEE, script}});
    }

    /** A transaction spending output 1 of `parent`. */
    CMutableTransaction Child(const CMutableTransaction& parent)
    {
        const CTxOut& spent{parent.vout[1]};
        return Spend(COutPoint{parent.GetHash(), 1}, spent, {CTxOut{0, CScript() << OP_RETURN}, CTxOut{spent.nValue - TX_FEE, script}});
    }

    bool InBlock(const CBlock& block, const CMutableTransaction& tx)
    {
        return std::any_of(block.vtx.begin(), block.vtx.end(), [&](const CTransactionRef& in_block) { return in_block->GetHash() == tx.GetHash(); });
    }

    std::optional<sidechain::Mainchain::Failure> Failure(const CBlock& block) { return Record().GetFailure(block.GetHash()); }
    bool Failed(const CBlock& block)
    {
        LOCK(::cs_main);
        const CBlockIndex* index{Chainman().m_blockman.LookupBlockIndex(block.GetHash())};
        return index && (index->nStatus & BLOCK_FAILED_VALID);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_chain_tests, SidechainChainSetup)

BOOST_AUTO_TEST_CASE(blocks_refused_by_kind)
{
    // A block that breaks a sidechain rule as the record stands is marked failed against the record:
    // the follower checks it again when its commitment comes back. One that breaks a rule whatever the
    // record says, or a rule that is not of the sidechain, is not.
    COutPoint coin;
    const CTxOut spent{Fund(10 * COIN, coin)};

    // A deposit the coinbase pays wrong: another output (the witness commitment) where the payout goes.
    Deposit(3 * COIN);
    CBlock misplaced{Build()};
    CMutableTransaction coinbase{*misplaced.vtx[0]};
    BOOST_REQUIRE(coinbase.vout.size() == 3 && coinbase.vout[1].nValue == 3 * COIN);
    coinbase.vout.erase(coinbase.vout.begin() + 1);
    misplaced.vtx[0] = MakeTransactionRef(coinbase);
    Finish(misplaced);
    Submit(misplaced);
    BOOST_CHECK_EQUAL(Reason(misplaced), "bad-sc-payout");
    BOOST_CHECK(Failed(misplaced));
    BOOST_CHECK(Failure(misplaced) == sidechain::Mainchain::Failure::RECORD);

    // A deposit the coinbase does not pay at all (nor has it a witness commitment, which it needs not).
    CBlock unpaid{Build()};
    coinbase = CMutableTransaction{*unpaid.vtx[0]};
    coinbase.vout.resize(1);
    coinbase.vin[0].scriptWitness.SetNull();
    unpaid.vtx[0] = MakeTransactionRef(coinbase);
    Finish(unpaid);
    Submit(unpaid);
    BOOST_CHECK_EQUAL(Reason(unpaid), "bad-sc-payouts-missing");
    BOOST_CHECK(Failed(unpaid));
    BOOST_CHECK(Failure(unpaid) == sidechain::Mainchain::Failure::RECORD);

    // A coinbase that takes more than the fees and the payouts: the sidechain rules hold, the coinbase
    // rule does not. There is no block subsidy; the payouts are what the coinbase may create.
    CBlock greedy{Build()};
    coinbase = CMutableTransaction{*greedy.vtx[0]};
    BOOST_REQUIRE(coinbase.vout.size() >= 2 && coinbase.vout[1].nValue == 3 * COIN);
    coinbase.vout[0].nValue += 1;
    greedy.vtx[0] = MakeTransactionRef(coinbase);
    Finish(greedy);
    Submit(greedy);
    BOOST_CHECK_EQUAL(Reason(greedy), "bad-cb-amount");
    BOOST_CHECK(Failed(greedy));
    BOOST_CHECK(!Failure(greedy));

    // A withdrawal that does not parse: wrong whatever the mainchain did.
    CBlock malformed{Build(/*use_mempool=*/false)};
    malformed.vtx.push_back(MakeTransactionRef(Spend(coin, spent, {CTxOut{COIN, sidechain::WithdrawalScript(COIN, key.GetPubKey().GetID(), script)}, CTxOut{spent.nValue - COIN - TX_FEE, script}})));
    node::RegenerateCommitments(malformed, Chainman());
    Finish(malformed);
    Submit(malformed);
    BOOST_CHECK_EQUAL(Reason(malformed), "bad-sc-withdrawal");
    BOOST_CHECK(Failed(malformed));
    BOOST_CHECK(!Failure(malformed));

    // The block that pays the deposit is taken.
    const CBlock paid{Mine()};
    BOOST_CHECK_EQUAL(paid.vtx[0]->vout[1].nValue, 3 * COIN);

    // A block the operator marks invalid is a failure of the operator's, which no change of the record
    // takes back; nor does judging blocks again under new parameters. The failures of the record are.
    BlockValidationState state;
    CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(paid.GetHash()))};
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index));
    BOOST_CHECK(Failure(paid) == sidechain::Mainchain::Failure::MANUAL);
    {
        LOCK(::cs_main);
        Chain().ResetAllBlockFailureFlags();
    }
    BOOST_CHECK(Failed(paid));
    BOOST_CHECK(!Failed(misplaced));
    BOOST_CHECK(!Failed(unpaid));
    BOOST_CHECK(!Failed(greedy));
    BOOST_CHECK(!Failed(malformed));
}

BOOST_AUTO_TEST_CASE(commitments_in_order)
{
    // Blind merged mining, as the headers see it: a block needs a commitment, and its parent one that
    // came before it.
    Mine();
    const CBlock uncommitted{Build()};
    Submit(uncommitted, /*commit=*/false);
    BOOST_CHECK_EQUAL(Reason(uncommitted), "bmm-unknown");
    // Not failed: the commitment may still come. The header waits for it.
    BOOST_CHECK(!Failed(uncommitted));
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().m_bmm_waiting.contains(uncommitted.GetHash())));

    // A block, and one on top of it, committed the other way round.
    const int base{Record().Height()};
    const CBlock first{Mine()};
    const CBlock second{Build()};
    Record().Truncate(base);
    Main([&](sidechain::MainBlock& b) { b.bmm = second.GetHash(); });
    Main([&](sidechain::MainBlock& b) { b.bmm = first.GetHash(); });
    Submit(second, /*commit=*/false);
    BOOST_CHECK_EQUAL(Reason(second), "bmm-order");
    // An invalid header: not even in the block index.
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(second.GetHash())));

    // A block whose parent has no commitment (any more).
    Record().Truncate(base);
    Main();
    const CBlock orphan{[&] {
        // On the parent as the block assembler made it: the record is behind the state of the tip, so
        // no block can be built (see below); this one is made by hand.
        CBlock block{second};
        block.nTime += 1;
        Finish(block);
        return block;
    }()};
    Submit(orphan);
    BOOST_CHECK_EQUAL(Reason(orphan), "bmm-prev-unknown");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(orphan.GetHash())));

    // The record behind the mainchain block the tip acted on: the block assembler cannot build on it.
    Record().Truncate(base - 1);
    BOOST_CHECK_EXCEPTION(Build(), std::runtime_error, HasReason{"the block does not fit the mainchain on record (bad-sc-main-height)"});
}

BOOST_AUTO_TEST_CASE(assembly_leaves_out_refund_of_paid_withdrawal)
{
    // A refund in the mempool of a withdrawal that the mainchain then paid (with a bundle of another
    // branch): the block leaves it out, with what spends it, rather than fail; the mempool drops it.
    COutPoint coin;
    const CTxOut spent{Fund(10 * COIN, coin)};
    const CMutableTransaction withdrawal{Withdraw(coin, spent, 2 * COIN)};
    BOOST_REQUIRE(Accept(withdrawal).m_result_type == MempoolAcceptResult::ResultType::VALID);
    const CBlock with_withdrawal{Mine()};
    BOOST_REQUIRE(InBlock(with_withdrawal, withdrawal));
    const COutPoint outpoint{withdrawal.GetHash(), 0};
    const CMutableTransaction refund{Refund(outpoint, withdrawal)};
    const CMutableTransaction child{Child(refund)};
    BOOST_REQUIRE(Accept(refund).m_result_type == MempoolAcceptResult::ResultType::VALID);
    BOOST_REQUIRE(Accept(child).m_result_type == MempoolAcceptResult::ResultType::VALID);

    // The mainchain pays it, with a bundle this branch never had.
    Main([&](sidechain::MainBlock& b) {
        sidechain::MainDeposit change;
        change.destination = drivechain::WITHDRAWAL_RETURN_DEST;
        change.bundle = uint256{0xb0};
        change.payouts.emplace_back(2 * COIN - MAIN_FEE, script);
        b.deposits.push_back(change);
    });
    const CBlock block{[&] {
        ASSERT_DEBUG_LOG("which breaks the sidechain rules in this block (bad-sc-refund-unknown)");
        return Mine();
    }()};
    BOOST_CHECK(!InBlock(block, refund));
    BOOST_CHECK(!InBlock(block, child));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chain().SideState().GetWithdrawal(outpoint)));
    // The fees of what was left out are not in the coinbase.
    BOOST_CHECK_EQUAL(block.vtx[0]->vout[0].nValue, 0);
    BOOST_CHECK(!InMempool(refund));
    BOOST_CHECK(!InMempool(child));
    // Nor does it come back.
    const auto again{Accept(refund)};
    BOOST_CHECK(again.m_result_type == MempoolAcceptResult::ResultType::INVALID);
    BOOST_CHECK_EQUAL(again.m_state.GetRejectReason(), "bad-sc-refund-unknown");
}

BOOST_AUTO_TEST_CASE(assembly_keeps_what_does_not_depend_on_a_dropped_transaction)
{
    // Leaving a transaction out (and what spends it) leaves the others in, with their fees in the
    // coinbase: a refund the mainchain made unknown goes, an unrelated payment stays.
    COutPoint coin, other_coin;
    const CTxOut spent{Fund(10 * COIN, coin)};
    const CTxOut other_spent{Fund(5 * COIN, other_coin)};
    const CMutableTransaction withdrawal{Withdraw(coin, spent, 2 * COIN)};
    BOOST_REQUIRE(Accept(withdrawal).m_result_type == MempoolAcceptResult::ResultType::VALID);
    BOOST_REQUIRE(InBlock(Mine(), withdrawal));
    const COutPoint outpoint{withdrawal.GetHash(), 0};
    const CMutableTransaction refund{Refund(outpoint, withdrawal)};
    const CMutableTransaction child{Child(refund)};
    // A payment with a fee of its own, unlike the others' (so that the coinbase tells whose it took).
    const CMutableTransaction payment{Spend(other_coin, other_spent, {CTxOut{other_spent.nValue - 3 * TX_FEE, script}})};
    BOOST_REQUIRE(Accept(refund).m_result_type == MempoolAcceptResult::ResultType::VALID);
    BOOST_REQUIRE(Accept(child).m_result_type == MempoolAcceptResult::ResultType::VALID);
    BOOST_REQUIRE(Accept(payment).m_result_type == MempoolAcceptResult::ResultType::VALID);

    Main([&](sidechain::MainBlock& b) {
        sidechain::MainDeposit change;
        change.destination = drivechain::WITHDRAWAL_RETURN_DEST;
        change.bundle = uint256{0xb1};
        change.payouts.emplace_back(2 * COIN - MAIN_FEE, script);
        b.deposits.push_back(change);
    });
    const CBlock block{[&] {
        ASSERT_DEBUG_LOG("which breaks the sidechain rules in this block (bad-sc-refund-unknown)");
        return Mine();
    }()};
    BOOST_CHECK(!InBlock(block, refund));
    BOOST_CHECK(!InBlock(block, child));
    BOOST_CHECK(InBlock(block, payment));
    BOOST_CHECK_EQUAL(block.vtx.size(), 2U);
    // The fee of the payment alone.
    BOOST_CHECK_EQUAL(block.vtx[0]->vout[0].nValue, 3 * TX_FEE);
    BOOST_CHECK(!InMempool(payment));
    BOOST_CHECK(!InMempool(refund));
    BOOST_CHECK(!InMempool(child));
}

BOOST_AUTO_TEST_CASE(assembly_bundle_goes_before_overdue_refund)
{
    // A bundle waits for refunds of its withdrawals in the mempool while they are recent. Once one has
    // waited REFUND_GRACE_BLOCKS, the bundle goes first: refunds of its withdrawals leave the block, with
    // what spends them, and the mempool.
    COutPoint coin;
    const CTxOut spent{Fund(10 * COIN, coin)};
    const CMutableTransaction withdrawal{Withdraw(coin, spent, 2 * COIN)};
    BOOST_REQUIRE(Accept(withdrawal).m_result_type == MempoolAcceptResult::ResultType::VALID);
    const CBlock with_withdrawal{Mine()};
    const int withdrawal_height{Tip()->nHeight};
    const COutPoint outpoint{withdrawal.GetHash(), 0};
    // The next block commits to a bundle; the mainchain lets it fail.
    Mine();
    const auto bundle{WITH_LOCK(::cs_main, return Chain().SideState().Bundle())};
    BOOST_REQUIRE(bundle);
    BOOST_CHECK(bundle->withdrawals == std::vector<COutPoint>{outpoint});
    // A refund cannot take it meanwhile.
    const CMutableTransaction early_refund{Refund(outpoint, withdrawal)};
    const auto in_bundle{Accept(early_refund, /*test_accept=*/true)};
    BOOST_CHECK_EQUAL(in_bundle.m_state.GetRejectReason(), "bad-sc-refund-in-bundle");
    Main([&](sidechain::MainBlock& b) { b.bundles.push_back({bundle->hash, false}); });
    Mine();
    const int failed_at{Tip()->nHeight};
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chain().SideState().LastFailureHeight()), failed_at);
    while (Tip()->nHeight + 1 - failed_at < Params().bundle_retry_delay) Mine();
    BOOST_REQUIRE_GT(Tip()->nHeight + 1 - withdrawal_height, sidechain::REFUND_GRACE_BLOCKS);
    // Now a refund comes, and a transaction that spends it.
    const CMutableTransaction refund{Refund(outpoint, withdrawal)};
    const CMutableTransaction child{Child(refund)};
    BOOST_REQUIRE(Accept(refund).m_result_type == MempoolAcceptResult::ResultType::VALID);
    BOOST_REQUIRE(Accept(child).m_result_type == MempoolAcceptResult::ResultType::VALID);
    // A second request for the same refund is one too many.
    const auto twice{Accept(Refund(outpoint, child), /*test_accept=*/true)};
    BOOST_CHECK_EQUAL(twice.m_state.GetRejectReason(), "sc-refund-in-mempool");
    const CBlock block{Mine()};
    const auto retry{WITH_LOCK(::cs_main, return Chain().SideState().Bundle())};
    BOOST_REQUIRE(retry);
    BOOST_CHECK(retry->hash != bundle->hash);
    BOOST_CHECK(retry->withdrawals == std::vector<COutPoint>{outpoint});
    BOOST_CHECK(!InBlock(block, refund));
    BOOST_CHECK(!InBlock(block, child));
    BOOST_CHECK(!InMempool(refund));
    BOOST_CHECK(!InMempool(child));
}

BOOST_AUTO_TEST_CASE(assembly_refunds_wait_for_a_recent_withdrawal)
{
    // While the withdrawals of the next bundle are recent, a refund of one of them in the mempool holds
    // the bundle back: the refund goes in the block, the bundle in a later one.
    COutPoint coin;
    const CTxOut spent{Fund(10 * COIN, coin)};
    const CMutableTransaction withdrawal{Withdraw(coin, spent, 2 * COIN)};
    BOOST_REQUIRE(Accept(withdrawal).m_result_type == MempoolAcceptResult::ResultType::VALID);
    Mine();
    const COutPoint outpoint{withdrawal.GetHash(), 0};
    const CMutableTransaction refund{Refund(outpoint, withdrawal)};
    BOOST_REQUIRE(Accept(refund).m_result_type == MempoolAcceptResult::ResultType::VALID);
    const CBlock block{Mine()};
    BOOST_CHECK(InBlock(block, refund));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chain().SideState().Bundle()));
    // The coinbase pays the refund: all that the withdrawal burned.
    BOOST_CHECK(block.vtx[0]->vout[1] == (CTxOut{2 * COIN, script}));
}

BOOST_AUTO_TEST_CASE(refund_held_back_by_a_bundle_pending_on_the_mainchain)
{
    // A bundle of another branch pending on the mainchain with support may hold the withdrawal: a refund
    // in the mempool waits, out of the block and then out of the mempool.
    COutPoint coin;
    const CTxOut spent{Fund(10 * COIN, coin)};
    const CMutableTransaction withdrawal{Withdraw(coin, spent, 2 * COIN)};
    BOOST_REQUIRE(Accept(withdrawal).m_result_type == MempoolAcceptResult::ResultType::VALID);
    Mine();
    const COutPoint outpoint{withdrawal.GetHash(), 0};
    // Signed by another key: refused, whatever else.
    CKey other;
    other.MakeNewKey(/*fCompressed=*/true);
    const auto forged{Accept(Refund(outpoint, withdrawal, &other), /*test_accept=*/true)};
    BOOST_CHECK_EQUAL(forged.m_state.GetRejectReason(), "bad-sc-refund-signature");
    // The refund is in the mempool before the next block, which would otherwise start a bundle.
    const CMutableTransaction refund{Refund(outpoint, withdrawal)};
    BOOST_REQUIRE(Accept(refund).m_result_type == MempoolAcceptResult::ResultType::VALID);
    Main([&](sidechain::MainBlock& b) {
        b.proposed.push_back(uint256{0xf0});
        b.pending.push_back({uint256{0xf0}, static_cast<uint32_t>(Params().pending_min_score)});
    });
    const CBlock block{Mine()};
    BOOST_CHECK(!InBlock(block, refund));
    BOOST_CHECK(!InMempool(refund));
    const auto again{Accept(refund, /*test_accept=*/true)};
    BOOST_CHECK_EQUAL(again.m_state.GetRejectReason(), "bad-sc-refund-bundle-pending");
}

BOOST_AUTO_TEST_CASE(mempool_withdrawal_rules)
{
    // The form of withdrawals, in the mempool as in blocks.
    COutPoint coin;
    const CTxOut spent{Fund(10 * COIN, coin)};
    const auto reason{[&](CAmount value, CAmount fee) {
        const auto result{Accept(Spend(coin, spent, {CTxOut{value, sidechain::WithdrawalScript(fee, key.GetPubKey().GetID(), script)}, CTxOut{spent.nValue - value - TX_FEE, script}}), /*test_accept=*/true)};
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID ? std::string{} : result.m_state.GetRejectReason();
    }};
    BOOST_CHECK_EQUAL(reason(COIN, COIN), "bad-sc-withdrawal");
    BOOST_CHECK_EQUAL(reason(Params().min_withdrawal - 1 + MAIN_FEE, MAIN_FEE), "bad-sc-withdrawal-amount");
    BOOST_CHECK_EQUAL(reason(Params().min_withdrawal + MAIN_FEE, MAIN_FEE), "");
    // A refund of a withdrawal there is not.
    const CMutableTransaction funding{Spend(coin, spent, {CTxOut{0, CScript() << OP_RETURN}, CTxOut{spent.nValue - TX_FEE, script}})};
    BOOST_REQUIRE(Accept(funding).m_result_type == MempoolAcceptResult::ResultType::VALID);
    const auto unknown{Accept(Refund(COutPoint{funding.GetHash(), 0}, funding), /*test_accept=*/true)};
    BOOST_CHECK_EQUAL(unknown.m_state.GetRejectReason(), "bad-sc-refund-unknown");
}

BOOST_AUTO_TEST_CASE(disconnect_without_undo_data)
{
    // A reorg deeper than the drivechain undo data reaches (DRIVECHAIN_UNDO_DEPTH): the block cannot be
    // taken back, the sidechain state of the block before it being derived from nowhere. It stays.
    Mine();
    const CBlock tip{Mine()};
    Chainman().m_blockman.m_drivechain_db->EraseBlockUndo(tip.GetHash());
    CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(tip.GetHash()))};
    BlockValidationState state;
    {
        ASSERT_DEBUG_LOG("no drivechain undo data for block " + tip.GetHash().ToString());
        BOOST_CHECK(!Chain().InvalidateBlock(state, index));
    }
    BOOST_CHECK(Tip()->GetBlockHash() == tip.GetHash());
}

BOOST_AUTO_TEST_CASE(headers_waiting_for_commitments_are_bounded)
{
    // Anyone can send headers no mainchain block will ever commit to: at most MAX_BMM_WAITING wait, and
    // past that the oldest of the peer that sent the most goes.
    LOCK(::cs_main);
    auto& waiting{Chainman().m_bmm_waiting};
    waiting.clear();
    const auto header{[](uint32_t n) {
        CBlockHeader h;
        h.nNonce = n;
        h.nTime = 7;
        return h;
    }};
    for (uint32_t i{0}; i < 600; ++i) Chainman().AddBmmWaiting(header(i), /*peer=*/1);
    for (uint32_t i{600}; i < 1000; ++i) Chainman().AddBmmWaiting(header(i), /*peer=*/2);
    BOOST_CHECK_EQUAL(waiting.size(), 1000U);
    Chainman().AddBmmWaiting(header(1000), /*peer=*/2);
    BOOST_CHECK_EQUAL(waiting.size(), 1000U);
    BOOST_CHECK(!waiting.contains(header(0).GetHash()));
    BOOST_CHECK(waiting.contains(header(1).GetHash()));
    BOOST_CHECK(waiting.contains(header(600).GetHash()));
    BOOST_CHECK(waiting.contains(header(1000).GetHash()));
    // A header validation looked at, without a peer, takes the peer that sends it later; a peer that
    // sends it again changes nothing.
    Chainman().AddBmmWaiting(header(2000), /*peer=*/-1);
    BOOST_CHECK_EQUAL(waiting.at(header(2000).GetHash()).peer, -1);
    Chainman().AddBmmWaiting(header(2000), /*peer=*/5);
    BOOST_CHECK_EQUAL(waiting.at(header(2000).GetHash()).peer, 5);
    Chainman().AddBmmWaiting(header(2000), /*peer=*/6);
    BOOST_CHECK_EQUAL(waiting.at(header(2000).GetHash()).peer, 5);
    waiting.clear();
}

BOOST_AUTO_TEST_CASE(duplicate_table_ids_refuse_to_load)
{
    // Two tables of the sidechain state under one key byte would read each other's entries: such a
    // build does not load a chainstate at all.
    const sidechain::Table<uint32_t, uint32_t> clash{'w'};
    const auto [status, error]{node::LoadChainstate(Chainman(), m_kernel_cache_sizes, node::ChainstateLoadOptions{})};
    BOOST_CHECK(status == node::ChainstateLoadStatus::FAILURE_FATAL);
    BOOST_CHECK_EQUAL(error.original, "Two tables of the sidechain state take the key byte 0x77: this build is broken");
}

namespace {
struct MainNetworkSetup : public TestingSetup {
    MainNetworkSetup() : TestingSetup{ChainType::MAIN} {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(block_weight_limits, MainNetworkSetup)
{
    // On the networks of this chain a block may weigh 6 million, of which the transactions other than
    // the coinbase 4 million: the rest is for the coinbase, which pays deposits and refunds.
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    BOOST_REQUIRE_EQUAL(params.max_block_weight, 6'000'000U);
    BOOST_REQUIRE_EQUAL(params.max_block_tx_weight, 4'000'000U);
    const auto with{[&](int count, size_t witness = 0) {
        auto block_template{interfaces::MakeMining(m_node)->createNewBlock({.coinbase_output_script = CScript() << OP_TRUE}, /*cooldown=*/false)};
        CBlock block{block_template->getBlock()};
        for (int i{0}; i < count; ++i) {
            // About 2.1 million weight units each, below the limit of a transaction.
            CMutableTransaction tx;
            tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0});
            tx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(525'000, 0x11));
            if (witness) tx.vin[0].scriptWitness.stack.emplace_back(witness, 0x22);
            block.vtx.push_back(MakeTransactionRef(std::move(tx)));
        }
        node::RegenerateCommitments(block, *m_node.chainman);
        LOCK(::cs_main);
        return TestBlockValidity(m_node.chainman->ActiveChainstate(), block, /*check_pow=*/false, /*check_merkle_root=*/true).GetRejectReason();
    }};
    // One such transaction is within both limits (it fails later, on its inputs); two are past that of
    // the transactions; with witnesses, past that of the block; three, past its size without witnesses.
    BOOST_CHECK_EQUAL(with(1), "bad-txns-inputs-missingorspent");
    BOOST_CHECK_EQUAL(with(2), "bad-blk-tx-weight");
    BOOST_CHECK_EQUAL(with(2, 1'000'000), "bad-blk-weight");
    BOOST_CHECK_EQUAL(with(3), "bad-blk-length");
}

namespace {
/** The parameters of a network as a release made from the template could have them: a first block of
 * its own (or the template's, own_genesis false), and the mainchain block that activated the sidechain,
 * or none (0). */
struct ReleaseParams : public CChainParams {
    ReleaseParams(const CChainParams& base, int main_activation_height, bool sidechain = true, bool own_genesis = true) : CChainParams{base}
    {
        consensus.sidechain.enabled = sidechain;
        consensus.sidechain.main_activation_height = main_activation_height;
        const std::string_view text{own_genesis ? "A sidechain" : "Sidechain template, its own network"};
        CMutableTransaction coinbase{*genesis.vtx[0]};
        coinbase.vin[0].scriptSig = CScript() << std::vector<unsigned char>{text.begin(), text.end()};
        genesis.vtx[0] = MakeTransactionRef(std::move(coinbase));
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(release_names_the_activation_of_its_slot, BasicTestingSetup)
{
    // A sidechain made from the template that does not say which mainchain block activated it: it does
    // not run on the main network; on a test network or signet it says so in the log.
    const auto main{CChainParams::Main()};
    const std::string slot{strprintf("This release names slot %u of the mainchain", main->GetConsensus().sidechain.slot)};
    {
        ASSERT_DEBUG_LOG(slot + " but not the height of the block that activated the sidechain there (SidechainParams::main_activation_height): it must not run on the main network.");
        BOOST_CHECK(!CheckSidechainActivation(ReleaseParams{*main, 0}));
    }
    for (const auto& network : {CChainParams::TestNet(), CChainParams::SigNet()}) {
        ASSERT_DEBUG_LOG(slot + " but not the height of the block that activated the sidechain there (SidechainParams::main_activation_height): set it once the slot activates.");
        BOOST_CHECK(CheckSidechainActivation(ReleaseParams{*network, 0}));
    }
    // Nothing to say: the height named; regtest (its slot is an option); not a sidechain; the
    // template's own networks, which no slot activated.
    auto quiet{DebugLogHelper{"This release names slot", [](const std::string* line) {
                                  BOOST_CHECK_MESSAGE(!line, "unexpected log line: " + (line ? *line : std::string{}));
                                  return false;
                              }}};
    BOOST_CHECK(CheckSidechainActivation(ReleaseParams{*main, 900}));
    BOOST_CHECK(CheckSidechainActivation(ReleaseParams{*CChainParams::TestNet(), 900}));
    BOOST_CHECK(CheckSidechainActivation(ReleaseParams{*CChainParams::RegTest(), 0}));
    BOOST_CHECK(CheckSidechainActivation(ReleaseParams{*main, 0, /*sidechain=*/false}));
    BOOST_CHECK(CheckSidechainActivation(ReleaseParams{*main, 0, /*sidechain=*/true, /*own_genesis=*/false}));
    BOOST_CHECK(CheckSidechainActivation(*main));
    BOOST_CHECK(CheckSidechainActivation(*CChainParams::TestNet()));
    BOOST_CHECK(CheckSidechainActivation(*CChainParams::SigNet()));
}

BOOST_AUTO_TEST_SUITE_END()
