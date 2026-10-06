// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <zside/orchard.h>
#include <zside/shielded.h>

#include <boost/test/unit_test.hpp>

#include <cstring>

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

BOOST_AUTO_TEST_SUITE_END()
