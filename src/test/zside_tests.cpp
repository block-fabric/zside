// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <consensus/params.h>
#include <consensus/tx_check.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <zside/marker.h>
#include <zside/orchard.h>
#include <zside/shielded.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
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
    // The networks: from their first block, but on the test network, until set at deploy.
    for (const auto chain : {ChainType::MAIN, ChainType::SIGNET, ChainType::REGTEST}) {
        BOOST_CHECK_EQUAL(CreateChainParams(m_args, chain)->GetConsensus().sidechain.zside.audit2_height, 0);
    }
    BOOST_CHECK_EQUAL(CreateChainParams(m_args, ChainType::TESTNET)->GetConsensus().sidechain.zside.audit2_height, 999999999);

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

BOOST_AUTO_TEST_SUITE_END()
