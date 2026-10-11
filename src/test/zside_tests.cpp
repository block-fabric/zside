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
#include <sidechain/mainchain.h>
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

/** The sighash domain the bundles of these tests are signed for, unless one says otherwise. */
const SighashDomain TEST_DOMAIN{.genesis = uint256::ONE, .slot = 6};
/** Another chain's. */
const SighashDomain OTHER_DOMAIN{.genesis = uint256::ZERO, .slot = 6};

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
CMutableTransaction ShieldingTx(CAmount value, const SighashDomain& domain, uint8_t seed_byte = 9, const std::optional<uint256>& anchor = std::nullopt)
{
    std::array<uint8_t, 32> seed{};
    seed.fill(seed_byte);
    std::array<uint8_t, 32> sk;
    BOOST_REQUIRE_EQUAL(zo_spending_key_from_seed(seed.data(), seed.size(), 1, 0, sk.data()), ZO_OK);
    std::array<uint8_t, FVK_SIZE> fvk;
    std::array<uint8_t, ADDRESS_SIZE> address;
    BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk.data(), fvk.data()), ZO_OK);
    BOOST_REQUIRE_EQUAL(zo_address(fvk.data(), 0, false, address.data()), ZO_OK);
    // Outputs only: the spends are dummies of no value, whose proof holds whatever the anchor.
    const uint256 root{anchor.value_or(EmptyRoot(TREE_DEPTH))};
    ZoBuilder* builder{zo_builder_new(root.data())};
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

/** Bytes of an action in an encoded bundle. */
constexpr size_t ACTION_BYTES{820};
/** The size of an Orchard proof for n actions. */
size_t ProofSize(size_t n) { return 2720 + 2272 * n; }

/** The bundle a marker carries: after OP_RETURN, the push header, the tag and the version. */
std::vector<uint8_t> MarkerBundle(const CScript& script)
{
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(script.GetOp(pc, opcode, data));
    BOOST_REQUIRE(data.size() > sizeof(SHIELDED_TAG));
    return {data.begin() + sizeof(SHIELDED_TAG) + 1, data.end()};
}

void WriteCompact(std::vector<uint8_t>& out, uint64_t n)
{
    if (n < 0xfd) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
        out.push_back(0xfd);
        for (int i{0}; i < 2; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
    } else {
        BOOST_REQUIRE(n <= 0xffffffff);
        out.push_back(0xfe);
        for (int i{0}; i < 4; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
    }
}

/**
 * A well-formed bundle of `n` actions, each a copy of the first action of `bundle` (a bundle of two
 * actions, as ShieldingTx makes), its value balance `balance`: it parses, its proof is all zeros and
 * its signatures are copies (it does not verify). Every action reveals the same nullifier.
 */
std::vector<uint8_t> CopiedActions(const std::vector<uint8_t>& bundle, size_t n, int64_t balance)
{
    // Two actions: the count (one byte), the actions, flags, value balance, anchor, proof, signatures.
    BOOST_REQUIRE_EQUAL(bundle.at(0), 2);
    const auto action{bundle.begin() + 1};
    const auto flags{action + 2 * ACTION_BYTES};
    const auto anchor{flags + 1 + 8};
    const auto sigs{anchor + 32 + 3 + ProofSize(2)};
    std::vector<uint8_t> out;
    WriteCompact(out, n);
    for (size_t i{0}; i < n; ++i) out.insert(out.end(), action, action + ACTION_BYTES);
    out.push_back(*flags);
    for (int i{0}; i < 8; ++i) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(balance) >> (8 * i)));
    out.insert(out.end(), anchor, anchor + 32);
    WriteCompact(out, ProofSize(n));
    out.resize(out.size() + ProofSize(n), 0);
    for (size_t i{0}; i < n; ++i) out.insert(out.end(), sigs, sigs + 64);
    out.insert(out.end(), sigs + 2 * 64, sigs + 3 * 64);
    BOOST_REQUIRE(sigs + 3 * 64 == bundle.end());
    return out;
}

/** A marker script whose push is written with `opcode`, not necessarily the minimal one. */
CScript MarkerWithPush(std::span<const uint8_t> bundle, opcodetype opcode)
{
    std::vector<uint8_t> data(std::begin(SHIELDED_TAG), std::end(SHIELDED_TAG));
    data.push_back(SHIELDED_VERSION);
    data.insert(data.end(), bundle.begin(), bundle.end());
    CScript script;
    script.push_back(OP_RETURN);
    script.push_back(opcode);
    const uint32_t size{static_cast<uint32_t>(data.size())};
    const int width{opcode == OP_PUSHDATA1 ? 1 : opcode == OP_PUSHDATA2 ? 2 : 4};
    for (int i{0}; i < width; ++i) script.push_back(static_cast<uint8_t>(size >> (8 * i)));
    script.insert(script.end(), data.begin(), data.end());
    return script;
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
    const CMutableTransaction good{ShieldingTx(1000, TEST_DOMAIN)};
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
        BOOST_CHECK(!state.ApplyTx(tx, reason, TEST_DOMAIN));
        BOOST_CHECK_EQUAL(reason, "bad-zs-value-balance");
    }
    // In range, but more than the pool holds: refused for the pool.
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{WithValueBalance(good, MAX_MONEY)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-pool-negative");
}

BOOST_AUTO_TEST_CASE(sighash_commits_to_the_chain)
{
    // The sighash commits to the genesis block and the slot of the chain.
    Consensus::SidechainParams params;
    params.slot = 6;
    const uint256 genesis{uint256::ONE};
    const SighashDomain domain{SighashDomainOf(params, genesis)};
    BOOST_CHECK(domain.genesis == genesis);
    BOOST_CHECK_EQUAL(domain.slot, 6U);
    // Each network: its own genesis block and slot.
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET, ChainType::REGTEST}) {
        const auto chain_params{CreateChainParams(m_args, chain)};
        const SighashDomain of_chain{SighashDomainOf(chain_params->GetConsensus())};
        BOOST_CHECK(of_chain.genesis == chain_params->GenesisBlock().GetHash());
        BOOST_CHECK_EQUAL(of_chain.slot, chain_params->GetConsensus().sidechain.slot);
    }

    // Another genesis, another slot: another sighash.
    const CMutableTransaction mtx{ShieldingTx(1000, domain)};
    const CTransaction tx{mtx};
    const uint256 commitment{uint256::ONE};
    const SighashDomain other_genesis{.genesis = uint256::ZERO, .slot = 6};
    const SighashDomain other_slot{.genesis = genesis, .slot = 7};
    const std::set<uint256> sighashes{ShieldedSighash(tx, 0, commitment, domain), ShieldedSighash(tx, 0, commitment, other_genesis),
                                      ShieldedSighash(tx, 0, commitment, other_slot)};
    BOOST_CHECK_EQUAL(sighashes.size(), 3U);

    // The bundle is valid in its domain alone, found valid there first or not (the cache of verified
    // bundles is by domain too).
    std::string reason;
    BOOST_CHECK(CheckShieldedTx(tx, reason, domain));
    BOOST_CHECK(!CheckShieldedTx(tx, reason, other_genesis));
    BOOST_CHECK_EQUAL(reason, "bad-zs-proof");
    BOOST_CHECK(!CheckShieldedTx(tx, reason, other_slot));
    BOOST_CHECK(CheckShieldedTx(tx, reason, domain));
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

BOOST_AUTO_TEST_CASE(bundles_of_a_block_in_one_batch)
{
    const SighashDomain domain{TEST_DOMAIN};
    const auto a{MakeTransactionRef(ShieldingTx(1000, domain, 1))};
    const auto b{MakeTransactionRef(ShieldingTx(2000, domain, 2))};
    const auto c{MakeTransactionRef(ShieldingTx(3000, domain, 3))};
    // One signed for another chain: the batch fails, and nothing of it is taken as verified.
    const auto bad{MakeTransactionRef(ShieldingTx(4000, OTHER_DOMAIN, 4))};
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
    BOOST_CHECK(!VerifyBundles(block, OTHER_DOMAIN));
}

BOOST_AUTO_TEST_CASE(block_with_a_shielded_transaction_twice)
{
    // A shielded transaction has no inputs, so a second copy of it in the same block spends nothing twice:
    // the block is refused for holding it twice (connecting it would otherwise add its outputs over
    // themselves). The bundle need not be valid: CheckBlock does not look at proofs.
    CMutableTransaction shielding{ShieldingTx(1000, TEST_DOMAIN)};
    shielding.vin.clear();
    const auto tx{MakeTransactionRef(shielding)};
    const auto other{MakeTransactionRef(ShieldingTx(2000, TEST_DOMAIN, 11))};
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

    const CTransaction tx{ShieldingTx(1000, TEST_DOMAIN, 5)};
    std::string reason;
    // In a block and in the mempool: the tree is never full.
    BOOST_CHECK(!state.ApplyTx(tx, reason, TEST_DOMAIN));
    BOOST_CHECK_EQUAL(reason, "bad-zs-tree-full");
    reason.clear();
    BOOST_CHECK(!state.CheckState(tx, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-tree-full");
    BOOST_CHECK_EQUAL(state.Tree().Size(), CAPACITY - 2);
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
    CMutableTransaction mtx{ShieldingTx(1000, TEST_DOMAIN)};
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


/** The orchard index, its steps called one by one: a commit is seen between the batch being filled and written. */
class SteppedOrchardIndex : public OrchardIndex
{
public:
    using OrchardIndex::OrchardIndex;
    using OrchardIndex::CustomAppend;
    using OrchardIndex::CustomCommit;
    using OrchardIndex::CustomCommitted;
    using OrchardIndex::GetDB;
};

BOOST_FIXTURE_TEST_CASE(orchard_index_readers_see_the_tree_being_committed, TestingSetup)
{
    // A commit puts the tree in a batch, which BaseIndex writes after: in between, readers (a payment
    // on an RPC thread, while the scheduler's thread commits) see the tree as it is, not the one on disk
    // of the commit before. A batch that is not written is written with the next commit.
    SteppedOrchardIndex index{interfaces::MakeChain(m_node), 1_MiB, /*f_memory=*/true};
    const auto take_in{[&](uint8_t seed) {
        CBlock block;
        block.nNonce = seed;
        block.vtx.push_back(MakeTransactionRef(ShieldingTx(1000, TEST_DOMAIN, seed)));
        const uint256 hash{block.GetHash()};
        interfaces::BlockInfo info{hash};
        info.data = &block;
        BOOST_REQUIRE(index.CustomAppend(info));
        std::string error;
        return Bundle::FromTx(*block.vtx[0], error)->Commitments();
    }};
    const auto first{take_in(1)};
    const auto tree{index.Tree()};
    BOOST_CHECK_EQUAL(tree.first, first.size());
    BOOST_REQUIRE(index.FindNotes(first));

    // Committed into a batch not written yet.
    CDBBatch batch{index.GetDB()};
    BOOST_REQUIRE(index.CustomCommit(batch));
    BOOST_CHECK(index.Tree() == tree);
    BOOST_CHECK(index.FindNotes(first));
    // A block taken in meanwhile: on top of it.
    const auto second{take_in(2)};
    const auto grown{index.Tree()};
    BOOST_CHECK_EQUAL(grown.first, first.size() + second.size());
    BOOST_CHECK(index.FindNotes(first));
    BOOST_CHECK(index.FindNotes(second));
    // Written: the same, read from disk.
    index.GetDB().WriteBatch(batch);
    index.CustomCommitted();
    BOOST_CHECK(index.Tree() == grown);
    BOOST_CHECK(index.FindNotes(first));
    BOOST_CHECK(index.FindNotes(second));

    // A batch never written (the write failed): still read, and written with the next commit.
    const auto third{take_in(3)};
    const auto last{index.Tree()};
    BOOST_CHECK_EQUAL(last.first, grown.first + third.size());
    CDBBatch lost{index.GetDB()};
    BOOST_REQUIRE(index.CustomCommit(lost));
    BOOST_CHECK(index.Tree() == last);
    BOOST_CHECK(index.FindNotes(third));
    CDBBatch next{index.GetDB()};
    BOOST_REQUIRE(index.CustomCommit(next));
    index.GetDB().WriteBatch(next);
    index.CustomCommitted();
    BOOST_CHECK(index.Tree() == last);
    BOOST_CHECK(index.FindNotes(first));
    BOOST_CHECK(index.FindNotes(third));
}

BOOST_AUTO_TEST_CASE(every_reject_reason_of_a_shielded_tx)
{
    // One transaction per reason CheckShieldedTx gives, each with the exact reason; the valid one passes.
    const CMutableTransaction good{ShieldingTx(1000, TEST_DOMAIN, 21)};
    const CScript& marker{good.vout[0].scriptPubKey};
    const std::vector<uint8_t> bundle{MarkerBundle(marker)};
    const auto with_marker{[&](const CScript& script) {
        CMutableTransaction mtx{good};
        mtx.vout[0].scriptPubKey = script;
        return mtx;
    }};
    const auto plus_output{[&](CTxOut out, bool before) {
        CMutableTransaction mtx{good};
        mtx.vout.insert(before ? mtx.vout.begin() : mtx.vout.end(), std::move(out));
        return mtx;
    }};
    CMutableTransaction no_inputs_no_marker;
    no_inputs_no_marker.vout.emplace_back(1, CScript{} << OP_TRUE);
    CMutableTransaction coinbase{good};
    coinbase.vin = {CTxIn{COutPoint{}, CScript{} << 1 << OP_0}};
    CMutableTransaction marker_value_low{good};
    --marker_value_low.vout[0].nValue;
    CMutableTransaction marker_value_high{good};
    ++marker_value_high.vout[0].nValue;
    // Taking out of the pool: the marker carries nothing.
    CMutableTransaction marker_value_on_unshield{WithValueBalance(good, 500)};
    marker_value_on_unshield.vout[0].nValue = 0;
    CMutableTransaction marker_value_unshield_ok{marker_value_on_unshield};
    ++marker_value_on_unshield.vout[0].nValue;
    // A byte of the proof changed: the sighash (which leaves the proof out) is the same, the proof is not.
    CMutableTransaction proof_changed{good};
    proof_changed.vout[0].scriptPubKey[proof_changed.vout[0].scriptPubKey.size() - 3 * 64 - 100] ^= 1;
    // The tag and the version, then nothing.
    CScript tag_only;
    tag_only << OP_RETURN << std::vector<unsigned char>(std::begin(SHIELDED_TAG), std::end(SHIELDED_TAG));
    CScript other_version{marker};
    other_version[1 + 3 + sizeof(SHIELDED_TAG)] = SHIELDED_VERSION + 1;
    CScript trailing{marker};
    trailing << OP_TRUE;
    std::vector<uint8_t> garbage(bundle.size(), 0x42);
    std::vector<uint8_t> extra_byte{bundle};
    extra_byte.push_back(0);
    std::vector<uint8_t> two_fifty_seven{CopiedActions(bundle, MAX_ACTIONS + 1, -1000)};

    struct Case {
        const char* name;
        CMutableTransaction tx;
        std::string reason;
    };
    const std::vector<Case> cases{
        {"valid", good, ""},
        {"no marker, transparent inputs", [&] { CMutableTransaction m{good}; m.vout[0] = CTxOut{1, CScript{} << OP_TRUE}; return m; }(), ""},
        {"two markers", plus_output(good.vout[0], false), "bad-zs-markers"},
        {"a bad marker, then a good one", plus_output(CTxOut{0, ShieldedMarkerScript(garbage)}, true), "bad-zs-markers"},
        {"a good marker, then a bad one", plus_output(CTxOut{0, ShieldedMarkerScript(garbage)}, false), "bad-zs-markers"},
        {"garbage for a bundle", with_marker(ShieldedMarkerScript(garbage)), "bad-zs-bundle"},
        {"a byte after the bundle", with_marker(ShieldedMarkerScript(extra_byte)), "bad-zs-bundle"},
        {"a push wider than needed", with_marker(MarkerWithPush(bundle, OP_PUSHDATA4)), "bad-zs-bundle"},
        {"another version", with_marker(other_version), "bad-zs-bundle"},
        {"the tag alone", with_marker(tag_only), "bad-zs-bundle"},
        {"an opcode after the push", with_marker(trailing), "bad-zs-bundle"},
        // More actions than MAX_ACTIONS: the bundle's parser refuses them before CheckShieldedTx counts them.
        {"257 actions", with_marker(ShieldedMarkerScript(two_fifty_seven)), "bad-zs-bundle"},
        {"no input, no marker", no_inputs_no_marker, "bad-txns-vin-empty"},
        {"in a coinbase", coinbase, "bad-zs-coinbase"},
        {"value balance below -MAX_MONEY", WithValueBalance(good, -MAX_MONEY - 1), "bad-zs-value-balance"},
        {"value balance above MAX_MONEY", WithValueBalance(good, MAX_MONEY + 1), "bad-zs-value-balance"},
        {"marker carries less than goes in", marker_value_low, "bad-zs-marker-value"},
        {"marker carries more than goes in", marker_value_high, "bad-zs-marker-value"},
        {"marker carries value out of the pool", marker_value_on_unshield, "bad-zs-marker-value"},
        // Its value balance changed: well formed, carried right, but not what was signed.
        {"value balance changed", marker_value_unshield_ok, "bad-zs-proof"},
        {"proof changed", proof_changed, "bad-zs-proof"},
        {"signed for another chain", ShieldingTx(1000, OTHER_DOMAIN, 21), "bad-zs-proof"},
    };
    for (const Case& c : cases) {
        std::string reason;
        const bool ok{CheckShieldedTx(CTransaction{c.tx}, reason, TEST_DOMAIN)};
        BOOST_CHECK_MESSAGE(ok == c.reason.empty(), c.name);
        if (!c.reason.empty()) {
            BOOST_CHECK_MESSAGE(reason == c.reason, c.name << ": " << reason);
            // A transaction's own reasons fail a block whatever the record of the mainchain; the
            // transparent rule is the template's.
            BOOST_CHECK_EQUAL(TxOnlyReason(reason), reason.starts_with("bad-zs-"));
        }
    }
}

BOOST_AUTO_TEST_CASE(too_many_actions_is_refused_by_the_parser)
{
    // CheckShieldedTx refuses a bundle of more than MAX_ACTIONS actions, but such a bundle never parses
    // (the library has the same limit): bad-zs-too-many-actions is defensive. The limit itself parses.
    const std::vector<uint8_t> bundle{MarkerBundle(ShieldingTx(1000, TEST_DOMAIN, 22).vout[0].scriptPubKey)};
    for (const size_t n : {size_t{1}, size_t{2}, size_t{252}, size_t{253}, MAX_ACTIONS}) {
        const auto parsed{Bundle::FromBytes(CopiedActions(bundle, n, -1000))};
        BOOST_REQUIRE_MESSAGE(parsed, n);
        BOOST_CHECK_EQUAL(parsed->NumActions(), n);
        BOOST_CHECK_EQUAL(parsed->ValueBalance(), -1000);
        BOOST_CHECK_EQUAL(parsed->Nullifiers().size(), n);
        BOOST_CHECK_EQUAL(parsed->Commitments().size(), n);
    }
    BOOST_CHECK(!Bundle::FromBytes(CopiedActions(bundle, MAX_ACTIONS + 1, -1000)));
    // No action at all, or the proof of another number of actions: neither parses.
    std::vector<uint8_t> none{CopiedActions(bundle, 1, 0)};
    none[0] = 0;
    BOOST_CHECK(!Bundle::FromBytes(none));
    std::vector<uint8_t> short_proof{CopiedActions(bundle, 3, 0)};
    const size_t proof_at{1 + 3 * ACTION_BYTES + 1 + 8 + 32};
    short_proof[proof_at + 1] = static_cast<uint8_t>(ProofSize(2));
    short_proof[proof_at + 2] = static_cast<uint8_t>(ProofSize(2) >> 8);
    short_proof.erase(short_proof.begin() + proof_at + 3, short_proof.begin() + proof_at + 3 + 2272);
    BOOST_CHECK(!Bundle::FromBytes(short_proof));
}

BOOST_AUTO_TEST_CASE(value_in_with_every_action_count_encoding)
{
    // ShieldedValueIn reads the value balance past the actions, whose count is a compact size.
    const std::vector<uint8_t> bundle{MarkerBundle(ShieldingTx(1000, TEST_DOMAIN, 23).vout[0].scriptPubKey)};
    CMutableTransaction mtx;
    for (const size_t n : {size_t{1}, size_t{252}, size_t{253}, MAX_ACTIONS}) {
        mtx.vout = {CTxOut{0, ShieldedMarkerScript(CopiedActions(bundle, n, 777))}};
        BOOST_CHECK_MESSAGE(ShieldedValueIn(CTransaction{mtx}) == 777, n);
    }
    // Bundles it cannot read: nothing.
    const auto value_in{[&](std::vector<uint8_t> payload) {
        mtx.vout = {CTxOut{0, ShieldedMarkerScript(payload)}};
        return ShieldedValueIn(CTransaction{mtx});
    }};
    // No bundle after the version.
    BOOST_CHECK_EQUAL(value_in({}), 0);
    // A compact size of three bytes cut short.
    BOOST_CHECK_EQUAL(value_in({0xfd, 0x01}), 0);
    // Compact sizes of five and nine bytes: more actions than any bundle has.
    std::vector<uint8_t> wide{CopiedActions(bundle, 1, 777)};
    wide[0] = 0xfe;
    BOOST_CHECK_EQUAL(value_in(wide), 0);
    wide[0] = 0xff;
    BOOST_CHECK_EQUAL(value_in(wide), 0);
    // More than MAX_ACTIONS, written as a compact size of three bytes.
    std::vector<uint8_t> too_many{CopiedActions(bundle, MAX_ACTIONS, 777)};
    too_many[1] = static_cast<uint8_t>(MAX_ACTIONS + 1);
    too_many[2] = static_cast<uint8_t>((MAX_ACTIONS + 1) >> 8);
    BOOST_CHECK_EQUAL(value_in(too_many), 0);
    // A marker that is no well-formed marker (a wider push than needed) is not read either.
    mtx.vout = {CTxOut{0, MarkerWithPush(CopiedActions(bundle, 1, 777), OP_PUSHDATA4)}};
    BOOST_CHECK_EQUAL(ShieldedValueIn(CTransaction{mtx}), 0);
    // The first marker that reads counts.
    mtx.vout = {CTxOut{1, CScript{} << OP_TRUE}, CTxOut{0, ShieldedMarkerScript(CopiedActions(bundle, 1, 5))}};
    BOOST_CHECK_EQUAL(ShieldedValueIn(CTransaction{mtx}), 5);
}

BOOST_AUTO_TEST_CASE(every_reject_reason_of_the_state)
{
    // The checks against the pool (CheckState, and ApplyTx after the proof), one per reason.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    ShieldedState state{store};
    BOOST_CHECK_EQUAL(state.AnchorCount(), 1U);
    BOOST_CHECK_EQUAL(state.Pool(), 0);
    const CMutableTransaction good{ShieldingTx(1000, TEST_DOMAIN, 31)};
    std::string reason;

    // A transaction it cannot read: the reason of the bundle.
    CMutableTransaction two_markers{good};
    two_markers.vout.push_back(good.vout[0]);
    BOOST_CHECK(!state.CheckState(CTransaction{two_markers}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-markers");
    CMutableTransaction garbage{good};
    garbage.vout[0].scriptPubKey = ShieldedMarkerScript(std::vector<uint8_t>(100, 0x42));
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{garbage}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-bundle");
    // No bundle: nothing to check.
    CMutableTransaction transparent{good};
    transparent.vout[0] = CTxOut{1, CScript{} << OP_TRUE};
    reason.clear();
    BOOST_CHECK(state.CheckState(CTransaction{transparent}, reason));
    BOOST_CHECK(state.ApplyTx(CTransaction{transparent}, reason, TEST_DOMAIN));
    BOOST_CHECK_EQUAL(state.Tree().Size(), 0U);

    // Two actions that reveal the same nullifier, in one bundle.
    CMutableTransaction twice{good};
    twice.vout[0].scriptPubKey = ShieldedMarkerScript(CopiedActions(MarkerBundle(good.vout[0].scriptPubKey), 2, -1000));
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{twice}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-double-spend");

    // An anchor that is no root the tree had.
    Frontier other_tree;
    other_tree.Append(Leaf(1));
    const CMutableTransaction unanchored{ShieldingTx(1000, TEST_DOMAIN, 32, other_tree.Root())};
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{unanchored}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-anchor");
    const uint256 before{sidechain::StoreHash(store)};
    reason.clear();
    BOOST_CHECK(!state.ApplyTx(CTransaction{unanchored}, reason, TEST_DOMAIN));
    BOOST_CHECK_EQUAL(reason, "bad-zs-anchor");
    BOOST_CHECK(sidechain::StoreHash(store) == before);

    // Applied: its nullifiers spent, its notes in the tree, its value in the pool.
    reason.clear();
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{good}, reason, TEST_DOMAIN), reason);
    BOOST_CHECK_EQUAL(state.Pool(), 1000);
    BOOST_CHECK_EQUAL(state.NullifierCount(), 2U);
    BOOST_CHECK_EQUAL(state.Tree().Size(), 2U);
    std::string error;
    for (const uint256& nf : Bundle::FromTx(CTransaction{good}, error)->Nullifiers()) BOOST_CHECK(state.IsSpent(nf));
    // Its tree's root is an anchor once the block ends, once only.
    BOOST_CHECK(!state.IsAnchor(state.Tree().Root()));
    state.EndBlock();
    BOOST_CHECK(state.IsAnchor(state.Tree().Root()));
    BOOST_CHECK_EQUAL(state.AnchorCount(), 2U);
    state.EndBlock();
    BOOST_CHECK_EQUAL(state.AnchorCount(), 2U);

    // Again: its nullifiers are spent.
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{good}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-double-spend");
    const uint256 applied{sidechain::StoreHash(store)};
    reason.clear();
    BOOST_CHECK(!state.ApplyTx(CTransaction{good}, reason, TEST_DOMAIN));
    BOOST_CHECK_EQUAL(reason, "bad-zs-double-spend");
    BOOST_CHECK(sidechain::StoreHash(store) == applied);

    // A bundle anchored at a root the tree had: taken.
    const CMutableTransaction anchored{ShieldingTx(500, TEST_DOMAIN, 33, state.Tree().Root())};
    reason.clear();
    BOOST_CHECK_MESSAGE(state.CheckState(CTransaction{anchored}, reason), reason);
    BOOST_CHECK_MESSAGE(state.ApplyTx(CTransaction{anchored}, reason, TEST_DOMAIN), reason);
    BOOST_CHECK_EQUAL(state.Pool(), 1500);

    // The proof is the last check of ApplyTx, before the pool: a bundle of another chain fails there.
    reason.clear();
    BOOST_CHECK(!state.ApplyTx(CTransaction{ShieldingTx(1000, OTHER_DOMAIN, 34)}, reason, TEST_DOMAIN));
    BOOST_CHECK_EQUAL(reason, "bad-zs-proof");
    // Out of the pool, more than it holds.
    reason.clear();
    BOOST_CHECK(!state.CheckState(CTransaction{WithValueBalance(ShieldingTx(1000, TEST_DOMAIN, 35), 1501)}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-zs-pool-negative");
    // As much as it holds: the pool may be emptied.
    reason.clear();
    BOOST_CHECK_MESSAGE(state.CheckState(CTransaction{WithValueBalance(ShieldingTx(1000, TEST_DOMAIN, 35), 1500)}, reason), reason);
}

BOOST_AUTO_TEST_CASE(shielded_rules_of_a_block)
{
    // State::ConnectBlock: the coinbase may not carry a bundle; each transaction is checked, in a
    // batch first; the block's final root is an anchor of the next.
    Consensus::SidechainParams params;
    params.enabled = true;
    params.slot = 6;
    const SighashDomain domain{SighashDomainOf(params, Params().GetConsensus().hashGenesisBlock)};
    sidechain::Mainchain mainchain;
    for (uint8_t h{0}; h < 3; ++h) {
        sidechain::MainBlock main;
        main.hash = uint256{static_cast<uint8_t>(h + 1)};
        main.prev_hash = mainchain.TipHash();
        BOOST_REQUIRE(mainchain.Append(main));
    }
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay chain{empty};
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back(COutPoint{});
    coinbase.vin[0].scriptSig = CScript{} << 1 << OP_0;
    coinbase.vout.emplace_back(0, CScript{} << OP_TRUE);
    const auto connect{[&](std::vector<CTransactionRef> txs, sidechain::StoreOverlay& out) {
        CBlock block;
        block.vtx = std::move(txs);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const sidechain::Mainchain::AssumeCommitted assumed{mainchain, 2};
        sidechain::State state{out};
        CAmount minted;
        std::string reason;
        return state.ConnectBlock(block, 1, params, mainchain, minted, reason) ? std::string{} : reason;
    }};
    const auto shield_a{MakeTransactionRef(ShieldingTx(1000, domain, 41))};
    const auto shield_b{MakeTransactionRef(ShieldingTx(2000, domain, 42))};

    // A coinbase with a marker, valid or not.
    CMutableTransaction marked_coinbase{coinbase};
    marked_coinbase.vout.push_back(shield_a->vout[0]);
    {
        sidechain::StoreOverlay block{static_cast<const sidechain::StoreView&>(chain)};
        BOOST_CHECK_EQUAL(connect({MakeTransactionRef(marked_coinbase)}, block), "bad-zs-coinbase");
    }
    // An invalid proof among valid ones: the batch fails, the transaction is found.
    {
        sidechain::StoreOverlay block{static_cast<const sidechain::StoreView&>(chain)};
        BOOST_CHECK_EQUAL(connect({MakeTransactionRef(coinbase), shield_a, MakeTransactionRef(ShieldingTx(3000, OTHER_DOMAIN, 43)), shield_b}, block), "bad-zs-proof");
    }
    // The same notes spent twice in one block (the same transaction twice, which CheckBlock refuses
    // before; the state refuses it too).
    {
        sidechain::StoreOverlay block{static_cast<const sidechain::StoreView&>(chain)};
        BOOST_CHECK_EQUAL(connect({MakeTransactionRef(coinbase), shield_a, shield_a}, block), "bad-zs-double-spend");
    }
    // Valid: the pool and the anchors move.
    uint256 root;
    {
        sidechain::StoreOverlay block{static_cast<const sidechain::StoreView&>(chain)};
        BOOST_CHECK_EQUAL(connect({MakeTransactionRef(coinbase), shield_a, shield_b}, block), "");
        const ShieldedState after{block};
        BOOST_CHECK_EQUAL(after.Pool(), 3000);
        BOOST_CHECK_EQUAL(after.Tree().Size(), 4U);
        root = after.Tree().Root();
        BOOST_CHECK(after.IsAnchor(root));
        BOOST_CHECK_EQUAL(after.AnchorCount(), 2U);
        block.MergeInto(chain);
    }
    // The next block: the same transaction again spends spent nullifiers.
    {
        sidechain::StoreOverlay block{static_cast<const sidechain::StoreView&>(chain)};
        BOOST_CHECK_EQUAL(connect({MakeTransactionRef(coinbase), shield_a}, block), "bad-zs-double-spend");
    }
}

BOOST_AUTO_TEST_CASE(no_transparent_inputs_flag)
{
    // Written with the flag of no inputs (2) only when there is a marker; read back the same.
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vout.emplace_back(0, ShieldedMarkerScript(std::vector<uint8_t>(10, 7)));
    DataStream s{};
    s << TX_WITH_WITNESS(mtx);
    const std::vector<std::byte> bytes(s.begin(), s.end());
    BOOST_CHECK_EQUAL(std::to_integer<int>(bytes[4]), 0);
    BOOST_CHECK_EQUAL(std::to_integer<int>(bytes[5]), 2);
    // Without witnesses, the old format: an empty vin is not followed by flags.
    DataStream legacy{};
    legacy << TX_NO_WITNESS(mtx);
    BOOST_CHECK_EQUAL(std::to_integer<int>(legacy[4]), 0);
    BOOST_CHECK_EQUAL(std::to_integer<int>(legacy[5]), 1);
    BOOST_CHECK_EQUAL(legacy.size() + 2, bytes.size());

    const auto read{[](std::vector<std::byte> b) -> std::string {
        DataStream in{b};
        CMutableTransaction out;
        try {
            in >> TX_WITH_WITNESS(out);
        } catch (const std::ios_base::failure& e) {
            return e.what();
        }
        return "";
    }};
    BOOST_CHECK_EQUAL(read(bytes), "");
    // An input after the flag of none: refused.
    CMutableTransaction with_input{mtx};
    with_input.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    DataStream normal{};
    normal << TX_NO_WITNESS(with_input);
    std::vector<std::byte> forged{bytes.begin(), bytes.begin() + 6};
    forged.insert(forged.end(), normal.begin() + 4, normal.end());
    BOOST_CHECK(read(forged).find("Unknown transaction optional data") != std::string::npos);
    // The flags of witnesses and of no inputs together: no witness to read, so a superfluous record.
    std::vector<std::byte> both{bytes};
    both[5] = std::byte{3};
    BOOST_CHECK(read(both).find("Superfluous witness record") != std::string::npos);
    // An unknown flag.
    std::vector<std::byte> unknown{bytes};
    unknown[5] = std::byte{4};
    BOOST_CHECK(read(unknown).find("Unknown transaction optional data") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
