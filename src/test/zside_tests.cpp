// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/tx_check.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <index/orchardindex.h>
#include <interfaces/chain.h>
#include <node/context.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <scheduler.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/byte_units.h>
#include <validation.h>
#include <zside/marker.h>
#include <zside/orchard.h>
#include <zside/shielded.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstring>
#include <future>
#include <limits>

using namespace zside;

namespace {
/** A leaf: a valid field element (small numbers are). */
uint256 Leaf(uint64_t n)
{
    uint256 leaf;
    std::memcpy(leaf.data(), &n, sizeof(n));
    return leaf;
}

/** The root of a tree, the naive way: every level in full, empty roots past the leaves. */
uint256 NaiveRoot(const std::vector<uint256>& leaves)
{
    std::vector<uint256> level{leaves};
    for (unsigned l{0}; l < TREE_DEPTH; ++l) {
        if (level.empty()) return EmptyRoot(TREE_DEPTH);
        if (level.size() % 2) level.push_back(EmptyRoot(l));
        std::vector<uint256> up;
        for (size_t i{0}; i < level.size(); i += 2) up.push_back(Combine(l, level[i], level[i + 1]));
        level = up;
    }
    return level.at(0);
}

/**
 * A transaction that puts `value` into the pool, its bundle (of two actions, one output) proved and
 * signed for `domain`; one transparent input (which no check here looks at).
 */
CMutableTransaction ShieldingTx(CAmount value, const std::optional<SighashDomain>& domain, uint8_t seed_byte = 9)
{
    std::array<uint8_t, 32> seed{};
    seed.fill(seed_byte);
    std::array<uint8_t, 32> sk;
    BOOST_REQUIRE_EQUAL(zo_spending_key_from_seed(seed.data(), seed.size(), 1, 0, sk.data()), ZO_OK);
    std::array<uint8_t, FVK_SIZE> fvk;
    std::array<uint8_t, ADDRESS_SIZE> address;
    BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk.data(), fvk.data()), ZO_OK);
    BOOST_REQUIRE_EQUAL(zo_address(fvk.data(), 0, false, address.data()), ZO_OK);
    const uint256 empty{EmptyRoot(TREE_DEPTH)};
    ZoBuilder* builder{zo_builder_new(empty.data())};
    BOOST_REQUIRE(builder);
    std::array<uint8_t, MEMO_SIZE> memo{};
    memo[0] = 0xF6;
    BOOST_REQUIRE_EQUAL(zo_builder_add_output(builder, fvk.data(), address.data(), value, memo.data()), ZO_OK);
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    BOOST_REQUIRE(unsigned_bundle);
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    // The marker, as a placeholder of the bundle's size: the sighash takes the commitment in its place.
    const size_t size{1 + 2 * 820 + 1 + 8 + 32 + 3 + 2720 + 2 * 2272 + 2 * 64 + 64};
    mtx.vout.emplace_back(value, ShieldedMarkerScript(std::vector<uint8_t>(size, 0)));
    uint256 commitment;
    BOOST_REQUIRE_EQUAL(zo_unsigned_commitment(unsigned_bundle, commitment.data()), ZO_OK);
    const uint256 sighash{ShieldedSighash(CTransaction{mtx}, 0, commitment, domain)};
    uint8_t* bytes{nullptr};
    size_t len{0};
    BOOST_REQUIRE_EQUAL(zo_unsigned_finish(unsigned_bundle, sighash.data(), &bytes, &len), ZO_OK);
    BOOST_REQUIRE_EQUAL(len, size);
    mtx.vout[0].scriptPubKey = ShieldedMarkerScript(std::span{bytes, len});
    zo_buffer_free(bytes, len);
    return mtx;
}

/** The bundle of a transaction, its value balance set to `balance` (no longer valid, but well formed). */
CMutableTransaction WithValueBalance(CMutableTransaction mtx, int64_t balance)
{
    std::string error;
    const auto bundle{Bundle::FromTx(CTransaction{mtx}, error)};
    BOOST_REQUIRE(bundle);
    // The marker: OP_RETURN, the push, the tag, the version, then the bundle.
    CScript& script{mtx.vout[bundle->marker_index].scriptPubKey};
    const size_t bundle_at{script.size() - (1 + 2 * 820 + 1 + 8 + 32 + 3 + 2720 + 2 * 2272 + 2 * 64 + 64)};
    const size_t at{bundle_at + 1 + 2 * 820 + 1};
    for (int i{0}; i < 8; ++i) script[at + i] = static_cast<uint8_t>(static_cast<uint64_t>(balance) >> (8 * i));
    return mtx;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(zside_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(frontier_matches_the_tree)
{
    Frontier frontier;
    std::vector<uint256> leaves;
    BOOST_CHECK(frontier.Root() == EmptyRoot(TREE_DEPTH));
    for (uint64_t n{1}; n <= 37; ++n) {
        frontier.Append(Leaf(n));
        leaves.push_back(Leaf(n));
        BOOST_CHECK_MESSAGE(frontier.Root() == NaiveRoot(leaves), "root after " << n << " leaves");
    }
    // Serialized and read back, it goes on the same.
    DataStream s{};
    s << frontier;
    Frontier copy;
    s >> copy;
    BOOST_CHECK(copy == frontier);
    copy.Append(Leaf(99));
    leaves.push_back(Leaf(99));
    BOOST_CHECK(copy.Root() == NaiveRoot(leaves));
}

BOOST_AUTO_TEST_CASE(no_transparent_inputs_round_trip)
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vout.emplace_back(0, ShieldedMarkerScript(std::vector<uint8_t>(100, 7)));
    mtx.nLockTime = 0;
    DataStream s{};
    s << TX_WITH_WITNESS(mtx);
    // The extended format, with the flag of no inputs.
    BOOST_CHECK_EQUAL(std::to_integer<int>(s[4]), 0);
    BOOST_CHECK_EQUAL(std::to_integer<int>(s[5]), 2);
    CMutableTransaction back;
    s >> TX_WITH_WITNESS(back);
    BOOST_CHECK(back.vin.empty());
    BOOST_CHECK(CTransaction{back}.GetHash() == CTransaction{mtx}.GetHash());
    // Transparent inputs after the flag of none are refused.
    CMutableTransaction with_input{mtx};
    with_input.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    DataStream bad{};
    bad << TX_WITH_WITNESS(mtx);
    std::vector<std::byte> bytes(bad.begin(), bad.end());
    // Put an input where the empty vin is: version | 00 02 | vin.
    bytes[6] = std::byte{1};
    DataStream forged{bytes};
    CMutableTransaction out;
    BOOST_CHECK_THROW(forged >> TX_WITH_WITNESS(out), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(blind_bundle_keeps_its_format)
{
    // A BIP300 blind bundle has no inputs either. Whatever its payouts, it keeps the format the
    // mainchain reads: a payout that looks a little like a marker (OP_RETURN OP_0 "ZORC") is no marker.
    CMutableTransaction blind;
    blind.version = 2;
    blind.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{0, 0, 0, 0, 0, 0, 3, 0xe8});
    CScript lookalike{CScript() << OP_RETURN << OP_0};
    const std::vector<unsigned char> tag{'Z', 'O', 'R', 'C', 0};
    lookalike.insert(lookalike.end(), tag.begin(), tag.end());
    blind.vout.emplace_back(COIN, lookalike);
    BOOST_CHECK(!IsShieldedMarker(lookalike));
    BOOST_CHECK(!CarriesShieldedBundle(blind));
    DataStream with{}, without{};
    with << TX_WITH_WITNESS(blind);
    without << TX_NO_WITNESS(blind);
    BOOST_CHECK(std::equal(with.begin(), with.end(), without.begin(), without.end()));
    // A marker too short to hold the tag is none either; a real one is.
    BOOST_CHECK(!CarriesShieldedBundle(CMutableTransaction{}));
    CMutableTransaction shielded;
    shielded.vout.emplace_back(0, ShieldedMarkerScript(std::vector<uint8_t>(3, 1)));
    BOOST_CHECK(IsShieldedMarker(shielded.vout[0].scriptPubKey) == CarriesShieldedBundle(shielded));
}

BOOST_AUTO_TEST_CASE(value_in_from_marker)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    BOOST_CHECK_EQUAL(ShieldedValueIn(CTransaction{mtx}), 0);
    // A bundle of one action: n, the action, flags, then the value balance.
    std::vector<uint8_t> bundle{1};
    bundle.resize(1 + 820 + 1, 0);
    for (int64_t value : {int64_t{12345}, int64_t{-12345}, MAX_MONEY + 1}) {
        std::vector<uint8_t> b{bundle};
        for (int i{0}; i < 8; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint64_t>(value) >> (8 * i)));
        mtx.vout = {CTxOut{0, ShieldedMarkerScript(b)}};
        // Only what comes out of the pool, and only in range.
        BOOST_CHECK_EQUAL(ShieldedValueIn(CTransaction{mtx}), value == 12345 ? 12345 : 0);
    }
    // Cut short: nothing.
    mtx.vout = {CTxOut{0, ShieldedMarkerScript(bundle)}};
    BOOST_CHECK_EQUAL(ShieldedValueIn(CTransaction{mtx}), 0);
}

BOOST_AUTO_TEST_CASE(bundle_through_the_interface)
{
    // A key, an address, a bundle that puts 1000 into the pool, proved and signed over a sighash.
    std::array<uint8_t, 32> seed{};
    seed.fill(9);
    std::array<uint8_t, 32> sk;
    BOOST_REQUIRE_EQUAL(zo_spending_key_from_seed(seed.data(), seed.size(), 1, 0, sk.data()), ZO_OK);
    std::array<uint8_t, FVK_SIZE> fvk;
    std::array<uint8_t, ADDRESS_SIZE> address;
    BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk.data(), fvk.data()), ZO_OK);
    BOOST_REQUIRE_EQUAL(zo_address(fvk.data(), 0, false, address.data()), ZO_OK);
    const uint256 empty{EmptyRoot(TREE_DEPTH)};
    ZoBuilder* builder{zo_builder_new(empty.data())};
    BOOST_REQUIRE(builder);
    std::array<uint8_t, MEMO_SIZE> memo{};
    memo[0] = 0xF6;
    BOOST_REQUIRE_EQUAL(zo_builder_add_output(builder, fvk.data(), address.data(), 1000, memo.data()), ZO_OK);
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    BOOST_REQUIRE(unsigned_bundle);
    BOOST_CHECK_EQUAL(zo_unsigned_value_balance(unsigned_bundle), -1000);
    uint256 sighash{uint256::ONE};
    uint8_t* bytes{nullptr};
    size_t len{0};
    BOOST_REQUIRE_EQUAL(zo_unsigned_finish(unsigned_bundle, sighash.data(), &bytes, &len), ZO_OK);
    std::vector<uint8_t> encoded(bytes, bytes + len);
    zo_buffer_free(bytes, len);
    const auto bundle{Bundle::FromBytes(encoded)};
    BOOST_REQUIRE(bundle);
    BOOST_CHECK_EQUAL(bundle->ValueBalance(), -1000);
    BOOST_CHECK_EQUAL(encoded.size(), 1 + 2 * 820 + 1 + 8 + 32 + 3 + 2720 + 2 * 2272 + 2 * 64 + 64);
    const ZoBundle* list[1]{bundle->get()};
    BOOST_CHECK(zo_bundles_verify(list, sighash.data(), 1));
    const uint256 other{uint256::ZERO};
    BOOST_CHECK(!zo_bundles_verify(list, other.data(), 1));
}

BOOST_AUTO_TEST_CASE(value_balance_range_before_the_pool)
{
    // A bundle whose value balance is far out of range, its proof not checked yet (the mempool checks
    // the state first): refused for the range, before the pool's balance is computed with it.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    ShieldedState state{store};
    const CMutableTransaction good{ShieldingTx(1000, std::nullopt)};
    std::string reason;
    BOOST_CHECK(state.CheckState(CTransaction{good}, reason));
    for (const int64_t balance : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), -MAX_MONEY - 1, MAX_MONEY + 1}) {
        const CTransaction tx{WithValueBalance(good, balance)};
        std::string error;
        const auto bundle{Bundle::FromTx(tx, error)};
        BOOST_REQUIRE(bundle);
        BOOST_CHECK_EQUAL(bundle->ValueBalance(), balance);
        reason.clear();
        BOOST_CHECK(!state.CheckState(tx, reason));
        BOOST_CHECK_EQUAL(reason, "bad-zs-value-balance");
        reason.clear();
        BOOST_CHECK(!state.ApplyTx(tx, reason, std::nullopt));
        BOOST_CHECK_EQUAL(reason, "bad-zs-value-balance");
    }
    // In range, but more than the pool holds: refused for the pool.
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{WithValueBalance(good, MAX_MONEY)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-pool-negative");
}

BOOST_AUTO_TEST_CASE(sighash_commits_to_the_chain)
{
    // From audit2_height, the sighash commits to the genesis block and the slot of the chain.
    Consensus::SidechainParams params;
    params.slot = 6;
    params.zside.audit2_height = 100;
    const uint256 genesis{uint256::ONE};
    BOOST_CHECK(!SighashDomainAt(params, genesis, 99));
    const auto domain{SighashDomainAt(params, genesis, 100)};
    BOOST_REQUIRE(domain);
    BOOST_CHECK(domain->genesis == genesis);
    BOOST_CHECK_EQUAL(domain->slot, 6U);
    BOOST_CHECK(SighashDomainAt(params, genesis, 1000));
    // The networks: from their first block, but on the test network, which ran before the rule: from
    // the height set at deploy (read here, not repeated: it moves with each deploy).
    for (const auto chain : {ChainType::MAIN, ChainType::SIGNET, ChainType::REGTEST}) {
        BOOST_CHECK_EQUAL(CreateChainParams(m_args, chain)->GetConsensus().sidechain.zside.audit2_height, 0);
    }
    const auto testnet{CreateChainParams(m_args, ChainType::TESTNET)};
    const int testnet_audit2{testnet->GetConsensus().sidechain.zside.audit2_height};
    BOOST_CHECK_GT(testnet_audit2, 0);
    BOOST_CHECK(!SighashDomainAt(testnet->GetConsensus(), testnet_audit2 - 1));
    BOOST_CHECK(SighashDomainAt(testnet->GetConsensus(), testnet_audit2));

    // Another genesis, another slot, none: another sighash.
    const CMutableTransaction mtx{ShieldingTx(1000, domain)};
    const CTransaction tx{mtx};
    const uint256 commitment{uint256::ONE};
    const SighashDomain other_genesis{.genesis = uint256::ZERO, .slot = 6};
    const SighashDomain other_slot{.genesis = genesis, .slot = 7};
    const std::set<uint256> sighashes{ShieldedSighash(tx, 0, commitment, domain), ShieldedSighash(tx, 0, commitment, other_genesis),
                                      ShieldedSighash(tx, 0, commitment, other_slot), ShieldedSighash(tx, 0, commitment, std::nullopt)};
    BOOST_CHECK_EQUAL(sighashes.size(), 4U);

    // The bundle is valid in its domain alone, found valid there first or not (the cache of verified
    // bundles is by domain too).
    std::string reason;
    BOOST_CHECK(CheckShieldedTx(tx, reason, domain));
    BOOST_CHECK(!CheckShieldedTx(tx, reason, other_genesis));
    BOOST_CHECK_EQUAL(reason, "bad-zs-proof");
    BOOST_CHECK(!CheckShieldedTx(tx, reason, other_slot));
    BOOST_CHECK(!CheckShieldedTx(tx, reason, std::nullopt));
    BOOST_CHECK(CheckShieldedTx(tx, reason, domain));
    const CTransaction before{ShieldingTx(1000, std::nullopt)};
    BOOST_CHECK(CheckShieldedTx(before, reason, std::nullopt));
    BOOST_CHECK(!CheckShieldedTx(before, reason, domain));
}

BOOST_AUTO_TEST_CASE(tx_only_reasons_fail_whatever_the_record)
{
    // The checks of a shielded transaction alone fail a block whatever the mainchain did: no
    // failure of the record (the follower would take it back, and look for a record that differs).
    for (const char* reason : {"bad-zs-markers", "bad-zs-bundle", "bad-zs-coinbase", "bad-zs-too-many-actions",
                               "bad-zs-value-balance", "bad-zs-marker-value", "bad-zs-proof"}) {
        BOOST_CHECK(TxOnlyReason(reason));
        BOOST_CHECK(sidechain::RecordIndependent(reason));
    }
    // Those of the state are left to depend on it, as any reason not listed.
    for (const char* reason : {"bad-zs-anchor", "bad-zs-double-spend", "bad-zs-tree-full", "bad-zs-pool-negative", "bad-zs-other"}) {
        BOOST_CHECK(!TxOnlyReason(reason));
        BOOST_CHECK(!sidechain::RecordIndependent(reason));
    }
    // The template's own are still there.
    BOOST_CHECK(sidechain::RecordIndependent("bad-sc-withdrawal"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-sc-payout"));
}

BOOST_AUTO_TEST_CASE(proof_failure_near_audit2_is_not_punished)
{
    // A peer whose tip is a few blocks from this node's signs in the other sighash domain near
    // audit2_height: that is no misbehaviour. A bundle valid in neither domain, or one sent far
    // from that height, is.
    Consensus::Params params;
    params.hashGenesisBlock = uint256::ONE;
    params.sidechain.slot = 6;
    params.sidechain.zside.audit2_height = 100;
    const auto domain{SighashDomainAt(params, 100)};
    BOOST_REQUIRE(domain);
    const CTransaction after{ShieldingTx(1000, domain, 7)};
    const CTransaction before{ShieldingTx(1000, std::nullopt, 8)};
    const CTransaction elsewhere{ShieldingTx(1000, SighashDomain{.genesis = uint256::ZERO, .slot = 6}, 10)};
    std::string reason;
    // Signed after the change, checked just before it (and the reverse): not punished.
    BOOST_CHECK(!CheckShieldedTx(after, reason, SighashDomainAt(params, 95)));
    BOOST_CHECK(!PunishableProofFailure(after, params, 95));
    BOOST_CHECK(!CheckShieldedTx(before, reason, SighashDomainAt(params, 105)));
    BOOST_CHECK(!PunishableProofFailure(before, params, 105));
    // Far from the change: punished.
    BOOST_CHECK(PunishableProofFailure(after, params, 100 - AUDIT2_GRACE_BLOCKS - 1));
    BOOST_CHECK(PunishableProofFailure(before, params, 100 + AUDIT2_GRACE_BLOCKS + 1));
    // Valid in neither domain: punished, near the change too.
    BOOST_CHECK(PunishableProofFailure(elsewhere, params, 95));
    BOOST_CHECK(PunishableProofFailure(elsewhere, params, 105));
    // A chain with the domain from its start has no other.
    params.sidechain.zside.audit2_height = 0;
    BOOST_CHECK(PunishableProofFailure(before, params, 1));
}

BOOST_AUTO_TEST_CASE(bundles_of_a_block_in_one_batch)
{
    const SighashDomain domain{.genesis = uint256::ONE, .slot = 6};
    const auto a{MakeTransactionRef(ShieldingTx(1000, domain, 1))};
    const auto b{MakeTransactionRef(ShieldingTx(2000, domain, 2))};
    const auto c{MakeTransactionRef(ShieldingTx(3000, domain, 3))};
    // One signed for another chain: the batch fails, and nothing of it is taken as verified.
    const auto bad{MakeTransactionRef(ShieldingTx(4000, std::nullopt, 4))};
    CMutableTransaction plain;
    plain.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 1});
    plain.vout.emplace_back(1, CScript() << OP_TRUE);
    const auto transparent{MakeTransactionRef(plain)};
    const std::vector<CTransactionRef> with_bad{c, transparent, bad};
    BOOST_CHECK(!VerifyBundles(with_bad, domain));
    std::string reason;
    BOOST_CHECK(CheckShieldedTx(*c, reason, domain));
    BOOST_CHECK(!CheckShieldedTx(*bad, reason, domain));
    BOOST_CHECK_EQUAL(reason, "bad-zs-proof");
    // All valid: verified together, then found so.
    const std::vector<CTransactionRef> block{a, transparent, b};
    BOOST_CHECK(VerifyBundles(block, domain));
    BOOST_CHECK(CheckShieldedTx(*a, reason, domain));
    BOOST_CHECK(CheckShieldedTx(*b, reason, domain));
    // In another domain they are not.
    BOOST_CHECK(!VerifyBundles(block, std::nullopt));
}

BOOST_AUTO_TEST_CASE(block_with_a_shielded_transaction_twice)
{
    // A shielded transaction has no inputs, so a second copy of it in the same block spends nothing twice:
    // the block is refused for holding it twice (connecting it would otherwise add its outputs over
    // themselves). The bundle need not be valid: CheckBlock does not look at proofs.
    CMutableTransaction shielding{ShieldingTx(1000, std::nullopt)};
    shielding.vin.clear();
    const auto tx{MakeTransactionRef(shielding)};
    const auto other{MakeTransactionRef(ShieldingTx(2000, std::nullopt, 11))};
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back(COutPoint{});
    coinbase.vin[0].scriptSig = CScript{} << 1 << OP_0;
    coinbase.vout.emplace_back(0, CScript{} << OP_TRUE);
    const auto cb{MakeTransactionRef(coinbase)};
    const auto check{[&](std::vector<CTransactionRef> txs) {
        CBlock block;
        block.vtx = std::move(txs);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        BlockValidationState state;
        CheckBlock(block, state, Params().GetConsensus(), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true);
        return state.IsValid() ? std::string{} : state.GetRejectReason();
    }};
    BOOST_CHECK_EQUAL(check({cb, tx}), "");
    // Neither layout is caught by the merkle tree's check for a mutated block.
    BOOST_CHECK_EQUAL(check({cb, tx, tx}), "bad-txns-duplicate");
    BOOST_CHECK_EQUAL(check({cb, tx, other, tx}), "bad-txns-duplicate");
}

BOOST_AUTO_TEST_CASE(tree_never_holds_two_to_the_32_leaves)
{
    // A tree two leaves short of 2^32 (its frontier written as the state keeps it): a bundle of two
    // actions would fill it, and a full tree's root is the empty tree's (no bit of its size is set).
    constexpr uint64_t CAPACITY{uint64_t{1} << TREE_DEPTH};
    Frontier nearly_full;
    DataStream stream;
    stream << uint64_t{CAPACITY - 2} << std::vector<uint256>(TREE_DEPTH, Leaf(1));
    stream >> nearly_full;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    sidechain::Cell<Frontier>{0x12, Frontier{}}.Put(store, nearly_full);
    ShieldedState state{store};
    BOOST_REQUIRE_EQUAL(state.Tree().Size(), CAPACITY - 2);

    const SighashDomain domain{.genesis = uint256::ONE, .slot = 6};
    const CTransaction after{ShieldingTx(1000, domain, 5)};
    const CTransaction before{ShieldingTx(1000, std::nullopt, 6)};
    std::string reason;
    // From audit2_height (a block in a sighash domain), and in the mempool: the tree is never full.
    BOOST_CHECK(!state.ApplyTx(after, reason, domain));
    BOOST_CHECK_EQUAL(reason, "bad-zs-tree-full");
    reason.clear();
    BOOST_CHECK(!state.CheckState(before, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-tree-full");
    BOOST_CHECK_EQUAL(state.Tree().Size(), CAPACITY - 2);
    // Before it, as the chain had it: it could be, and its root then was the empty tree's.
    reason.clear();
    BOOST_CHECK(state.ApplyTx(before, reason, std::nullopt));
    BOOST_CHECK_EQUAL(state.Tree().Size(), CAPACITY);
    BOOST_CHECK(state.Tree().Root() == EmptyRoot(TREE_DEPTH));
}

namespace {
struct RegTestSetup : public BasicTestingSetup {
    RegTestSetup() : BasicTestingSetup{ChainType::REGTEST} {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(no_inputs_only_on_a_sidechain, RegTestSetup)
{
    // Regtest (without -sidechainslot) is no sidechain: a transaction with a marker and no transparent
    // input passes the checks that know no chain, and not those of the chain.
    BOOST_REQUIRE(!Params().GetConsensus().sidechain.enabled);
    CMutableTransaction mtx{ShieldingTx(1000, std::nullopt)};
    mtx.vin.clear();
    mtx.vout[0].nValue = 0;
    const CTransaction tx{mtx};
    TxValidationState state;
    BOOST_CHECK(CheckTransaction(tx, state));
    CCoinsViewCache view{&CoinsViewEmpty::Get()};
    CAmount fee;
    BOOST_CHECK(!Consensus::CheckTxInputs(tx, state, view, 1, fee));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-vin-empty");
}

BOOST_FIXTURE_TEST_CASE(witnesses_never_wait_on_the_scheduler, TestChain100Setup)
{
    // The scheduler's thread runs the validation queue: there, finding notes must not wait for the
    // index to take a queued block (it would wait for itself, and the node would stop), nor may
    // waiting for the index (a melt/cast, a payment) ever block.
    g_orchardindex = std::make_unique<OrchardIndex>(interfaces::MakeChain(m_node), 1_MiB, /*f_memory=*/true);
    BOOST_REQUIRE(g_orchardindex->Init());
    g_orchardindex->Sync();
    const auto chain{interfaces::MakeChain(m_node)};
    const int height{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height())};
    BOOST_CHECK(chain->getOrchardWitnesses({}, height));
    BOOST_CHECK(!chain->getOrchardWitnesses({}, height + 1));

    // The scheduler's thread held, a block connected meanwhile: its notification waits behind.
    std::promise<void> holding, connected;
    std::promise<std::pair<bool, bool>> found;
    auto holding_future{holding.get_future()};
    auto connected_future{connected.get_future()};
    auto found_future{found.get_future()};
    m_node.scheduler->scheduleFromNow([&] {
        holding.set_value();
        connected_future.wait();
        chain->waitForOrchardIndex();
        found.set_value({chain->getOrchardWitnesses({}, height).has_value(), chain->getOrchardWitnesses({}, height + 1).has_value()});
    }, std::chrono::milliseconds{0});
    holding_future.wait();
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height()), height + 1);
    connected.set_value();
    BOOST_REQUIRE(found_future.wait_for(std::chrono::seconds{60}) == std::future_status::ready);
    const auto [at_old, at_new]{found_future.get()};
    // The index as it was: the tree of the block it has, not yet that of the new one ("try again").
    BOOST_CHECK(at_old);
    BOOST_CHECK(!at_new);

    // Off the scheduler's thread, the wait is a real one: then the new block's tree is found.
    chain->waitForOrchardIndex();
    BOOST_CHECK(chain->getOrchardWitnesses({}, height + 1));
    BOOST_CHECK(chain->getOrchardWitnesses({}, height));
    BOOST_CHECK(chain->getOrchardWitnesses({}, std::nullopt));
    BOOST_CHECK(!chain->getOrchardWitnesses({uint256::ONE}, std::nullopt));

    g_orchardindex->Stop();
    g_orchardindex.reset();
}

BOOST_AUTO_TEST_SUITE_END()
