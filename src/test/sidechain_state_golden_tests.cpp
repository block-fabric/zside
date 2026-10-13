// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Golden values of the sidechain rules: a fixed sequence of blocks, connected one after another to a
// state of their own, and the hash of the state and of what each coinbase pays out after each block.
// The values were recorded with the hooks of sidechain/hooks.h off, as they are unless a sidechain
// installs its own: any change in what a block does to the state, or in the order of what its coinbase
// pays, fails here. A change meant to make one has to say so, and record the values again.
//
// Orchard proofs are not deterministic: the shielded transactions are fixed in
// data/zside_golden_txs.json, which make_shielded_fixture (disabled; run it by name) made.

#include <addresstype.h>
#include <arith_uint256.h>
#include <chainparams.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <core_io.h>
#include <hash.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/data/zside_golden_txs.json.h>
#include <test/util/json.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <zside/orchard.h>
#include <zside/shielded.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace zside;

namespace {

/** A sidechain state, and the record of its mainchain, that blocks are connected to one after another. */
struct GoldenChain {
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    sidechain::Mainchain mainchain;
    uint256 prev{uint256{0xab}};
    int height{0};
    //! What the coinbase of the last block connected paid out, after its first output.
    std::vector<CTxOut> paid;

    GoldenChain() { Main(); }

    /** Append a block to the record of the mainchain. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int h{mainchain.Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x10000 + h)} << 128);
        block.prev_hash = mainchain.TipHash();
        block.time = 1'700'000'000 + h;
        if (fill) fill(block);
        BOOST_REQUIRE(mainchain.Append(block));
    }

    /**
     * A block of these transactions on the last one, after a mainchain block with these events, its
     * coinbase paying what the rules want (worked out on an overlay that is dropped, as a miner does)
     * and carrying `extra` after that; then a mainchain block that commits to it. Connected if valid;
     * why not, if not.
     */
    std::string Connect(const std::vector<CTransactionRef>& txs, const std::function<void(sidechain::MainBlock&)>& events = {}, const std::vector<CTxOut>& extra = {})
    {
        Main(events);
        CBlock block;
        block.nVersion = 0x20000000;
        block.hashPrevBlock = prev;
        block.nTime = 1'700'000'000 + height + 1;
        block.nBits = 0x207fffff;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(coinbase));
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        {
            sidechain::StoreOverlay scratch{store, /*journal=*/false};
            sidechain::State dry{scratch};
            std::vector<CTxOut> payouts, tx_payouts;
            std::string reason;
            if (dry.ApplyMainEvents(mainchain.Height(), mainchain, height + 1, params, payouts, reason) &&
                dry.ApplyTxs(block, height + 1, params, dry.MainPending(mainchain, params), /*peg_step=*/true, tx_payouts, reason)) {
                for (const CTxOut& out : dry.TakePayouts(std::move(payouts), std::move(tx_payouts))) coinbase.vout.push_back(out);
            }
        }
        const size_t payouts{coinbase.vout.size() - 1};
        coinbase.vout.insert(coinbase.vout.end(), extra.begin(), extra.end());
        block.vtx[0] = MakeTransactionRef(coinbase);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        sidechain::StoreOverlay block_store{store, /*journal=*/false};
        sidechain::State block_state{block_store};
        CAmount minted{0};
        std::string reason;
        if (!block_state.ConnectBlock(block, height + 1, params, mainchain, minted, reason)) return reason;
        block_store.MergeInto(store);
        store.TakeUndo();
        ++height;
        prev = block.GetHash();
        paid.assign(coinbase.vout.begin() + 1, coinbase.vout.begin() + 1 + payouts);
        return {};
    }

    /** The hash of what the last block paid out, in order. */
    uint256 PaidHash() const
    {
        HashWriter hasher{};
        hasher << paid;
        return hasher.GetHash();
    }

    /** The state and what the last block paid out are those recorded. */
    void Check(const std::string& state_hash, const std::string& paid_hash, size_t paid_count)
    {
        BOOST_CHECK_EQUAL(state.Hash().GetHex(), state_hash);
        BOOST_CHECK_EQUAL(PaidHash().GetHex(), paid_hash);
        BOOST_CHECK_EQUAL(paid.size(), paid_count);
    }

    ShieldedState Shielded() const { return ShieldedState{static_cast<const sidechain::StoreView&>(store)}; }
};

/** The sidechain parameters of the tests: a sidechain in slot 6, which the sighash domain names. */
Consensus::SidechainParams GoldenParams()
{
    Consensus::SidechainParams params;
    params.enabled = true;
    params.slot = 6;
    return params;
}

/** The key of the tests: always the same, so that its signatures are too. */
CKey FixedKey(uint8_t n)
{
    std::vector<unsigned char> secret(32, 0);
    secret[0] = 0x5e;
    secret[31] = n;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    return key;
}

CTxOut RefundOut(const CKey& key, const COutPoint& withdrawal)
{
    sidechain::RefundRequest request;
    request.withdrawal = withdrawal;
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(withdrawal)), request.signature));
    return CTxOut{0, sidechain::RefundScript(request)};
}

/** The shielded account of the tests. */
struct ShieldedAccount {
    std::array<uint8_t, 32> sk;
    std::array<uint8_t, FVK_SIZE> fvk;
    std::array<uint8_t, ADDRESS_SIZE> address;
    ShieldedAccount()
    {
        std::array<uint8_t, 32> seed;
        seed.fill(0x47);
        BOOST_REQUIRE_EQUAL(zo_spending_key_from_seed(seed.data(), seed.size(), 1, 0, sk.data()), ZO_OK);
        BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk.data(), fvk.data()), ZO_OK);
        BOOST_REQUIRE_EQUAL(zo_address(fvk.data(), 0, false, address.data()), ZO_OK);
    }
};

/** A note of the account, where it is in the tree. */
struct OwnNote {
    std::array<uint8_t, NOTE_SIZE> note;
    uint32_t position;
};

/** The path of the leaf at `position` to the root of a tree of these leaves. */
std::vector<uint8_t> PathOf(const std::vector<uint256>& leaves, uint32_t position)
{
    std::vector<uint8_t> path;
    std::vector<uint256> level{leaves};
    uint64_t index{position};
    for (unsigned l{0}; l < TREE_DEPTH; ++l) {
        const uint64_t sibling{index ^ 1};
        const uint256 node{sibling < level.size() ? level[sibling] : EmptyRoot(l)};
        path.insert(path.end(), node.begin(), node.end());
        if (level.size() % 2) level.push_back(EmptyRoot(l));
        std::vector<uint256> up;
        for (size_t i{0}; i < level.size(); i += 2) up.push_back(Combine(l, level[i], level[i + 1]));
        level = up;
        index >>= 1;
    }
    return path;
}

/**
 * A transaction with a bundle anchored at `anchor` that spends `spends` and pays `outputs` to the
 * account, proved and signed for `domain`; one transparent input, of `salt`. The marker carries what
 * goes into the pool.
 */
CMutableTransaction BundleTx(const ShieldedAccount& account, const uint256& anchor, const std::vector<OwnNote>& spends, const std::vector<uint256>& leaves,
                             const std::vector<CAmount>& outputs, const SighashDomain& domain, uint8_t salt)
{
    ZoBuilder* builder{zo_builder_new(anchor.data())};
    BOOST_REQUIRE(builder);
    CAmount in{0}, out{0};
    for (const OwnNote& spend : spends) {
        const std::vector<uint8_t> path{PathOf(leaves, spend.position)};
        BOOST_REQUIRE_EQUAL(zo_builder_add_spend(builder, account.sk.data(), spend.note.data(), spend.position, path.data()), ZO_OK);
        in += static_cast<CAmount>(zo_note_value(spend.note.data()));
    }
    std::array<uint8_t, MEMO_SIZE> memo{};
    memo[0] = 0xF6;
    for (const CAmount value : outputs) {
        BOOST_REQUIRE_EQUAL(zo_builder_add_output(builder, account.fvk.data(), account.address.data(), static_cast<uint64_t>(value), memo.data()), ZO_OK);
        out += value;
    }
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    BOOST_REQUIRE(unsigned_bundle);
    const size_t actions{zo_unsigned_num_actions(unsigned_bundle)};
    BOOST_REQUIRE_EQUAL(actions, 2U);
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{salt}), 0});
    // The marker, as a placeholder of the bundle's size: the sighash takes the commitment in its place.
    const size_t size{1 + 2 * 820 + 1 + 8 + 32 + 3 + 2720 + 2 * 2272 + 2 * 64 + 64};
    mtx.vout.emplace_back(std::max<CAmount>(0, out - in), ShieldedMarkerScript(std::vector<uint8_t>(size, 0)));
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

/** The note of the account of `value` that a transaction pays, where it lands after `leaves_before` leaves. */
OwnNote NoteOf(const ShieldedAccount& account, const CTransaction& tx, CAmount value, size_t leaves_before)
{
    std::string error;
    const auto bundle{Bundle::FromTx(tx, error)};
    BOOST_REQUIRE(bundle);
    for (size_t i{0}; i < bundle->NumActions(); ++i) {
        OwnNote found;
        std::array<uint8_t, MEMO_SIZE> memo;
        bool internal{false};
        if (zo_bundle_decrypt(bundle->get(), i, account.fvk.data(), found.note.data(), memo.data(), &internal) &&
            zo_note_value(found.note.data()) == static_cast<uint64_t>(value)) {
            found.position = static_cast<uint32_t>(leaves_before + i);
            return found;
        }
    }
    BOOST_FAIL("no such note");
    return {};
}

std::vector<uint256> LeavesOf(const std::vector<CMutableTransaction>& txs)
{
    std::vector<uint256> leaves;
    for (const CMutableTransaction& tx : txs) {
        std::string error;
        const auto bundle{Bundle::FromTx(CTransaction{tx}, error)};
        BOOST_REQUIRE(bundle);
        for (const uint256& cmx : bundle->Commitments()) leaves.push_back(cmx);
    }
    return leaves;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_state_golden_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(make_shielded_fixture, *boost::unit_test::disabled())
{
    // The shielded transactions of shielded_spends_across_blocks, printed as data/zside_golden_txs.json
    // wants them. Block 1: two notes in (A, B); block 2: A spent (anchored at the root block 1 left),
    // a note in (C); block 3: B spent at that same older root, the note A' that spent A at the root of
    // block 2.
    zo_init_prover();
    const SighashDomain domain{SighashDomainOf(GoldenParams(), Params().GetConsensus().hashGenesisBlock)};
    const ShieldedAccount account;
    const uint256 empty_root{EmptyRoot(TREE_DEPTH)};
    std::vector<CMutableTransaction> txs;
    txs.push_back(BundleTx(account, empty_root, {}, {}, {1000}, domain, 1));
    txs.push_back(BundleTx(account, empty_root, {}, {}, {2000}, domain, 2));
    const std::vector<uint256> leaves1{LeavesOf({txs[0], txs[1]})};
    const uint256 root1{[&] { Frontier f; for (const uint256& l : leaves1) f.Append(l); return f.Root(); }()};
    const OwnNote a{NoteOf(account, CTransaction{txs[0]}, 1000, 0)};
    const OwnNote b{NoteOf(account, CTransaction{txs[1]}, 2000, 2)};
    txs.push_back(BundleTx(account, root1, {a}, leaves1, {1000}, domain, 3));
    txs.push_back(BundleTx(account, empty_root, {}, {}, {3000}, domain, 4));
    std::vector<uint256> leaves2{leaves1};
    for (const uint256& l : LeavesOf({txs[2], txs[3]})) leaves2.push_back(l);
    const uint256 root2{[&] { Frontier f; for (const uint256& l : leaves2) f.Append(l); return f.Root(); }()};
    const OwnNote a2{NoteOf(account, CTransaction{txs[2]}, 1000, leaves1.size())};
    txs.push_back(BundleTx(account, root1, {b}, leaves1, {2000}, domain, 5));
    txs.push_back(BundleTx(account, root2, {a2}, leaves2, {1000}, domain, 6));
    UniValue out{UniValue::VARR};
    for (const CMutableTransaction& tx : txs) out.push_back(EncodeHexTx(CTransaction{tx}));
    std::cout << out.write(1) << std::endl;
}

BOOST_AUTO_TEST_CASE(shielded_spends_across_blocks)
{
    // Notes put into the pool, spent in later blocks at the anchors earlier blocks left (each block
    // that changes the tree leaves one, EndBlock), a note spent twice, a coinbase with a bundle; with a
    // deposit, a withdrawal and its refund for the coinbase to pay.
    GoldenChain chain;
    chain.params = GoldenParams();
    const CKey key{FixedKey(1)};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    std::vector<CTransactionRef> txs;
    const UniValue fixture{read_json(json_tests::zside_golden_txs)};
    for (const UniValue& hex : fixture.getValues()) {
        CMutableTransaction mtx;
        BOOST_REQUIRE(DecodeHexTx(mtx, hex.get_str()));
        txs.push_back(MakeTransactionRef(std::move(mtx)));
    }
    BOOST_REQUIRE_EQUAL(txs.size(), 6U);
    CMutableTransaction withdraw;
    withdraw.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{0x71}), 0});
    withdraw.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), pay));
    const CTransactionRef withdrawal{MakeTransactionRef(std::move(withdraw))};
    const auto deposit{[](sidechain::MainBlock& b) {
        sidechain::MainDeposit d;
        d.destination = "deposit";
        d.amount = 5 * COIN;
        d.txid = uint256{0xd1};
        b.deposits.push_back(d);
    }};

    // Block 1: two notes in, a deposit, a withdrawal.
    BOOST_CHECK_EQUAL(chain.Shielded().AnchorCount(), 1U);
    BOOST_REQUIRE_EQUAL(chain.Connect({txs[0], txs[1], withdrawal}, deposit), "");
    chain.Check("1fa19eeedd0098dbba07587c3fea1f2d25d6cd99311b024f5e79a8fbb6c93415", "7382c6d27934e5fa2e1da7140e784cbf08a8cf755594c3448a27ec4cbf3b2184", 1);
    const uint256 root1{chain.Shielded().Tree().Root()};
    BOOST_CHECK_EQUAL(chain.Shielded().Pool(), 3000);
    BOOST_CHECK_EQUAL(chain.Shielded().AnchorCount(), 2U);
    BOOST_CHECK(chain.Shielded().IsAnchor(root1));

    // A coinbase that carries a bundle: not valid, and nothing changes.
    const uint256 before{chain.state.Hash()};
    BOOST_CHECK_EQUAL(chain.Connect({}, {}, {txs[3]->vout[0]}), "bad-zs-coinbase");
    BOOST_CHECK(chain.state.Hash() == before);

    // Block 2: A spent, at the root block 1 left; a note in at the empty root; the refund.
    const CTransactionRef refund{MakeTransactionRef([&] {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{0x72}), 0});
        tx.vout.push_back(RefundOut(key, COutPoint{withdrawal->GetHash(), 0}));
        return tx;
    }())};
    BOOST_REQUIRE_EQUAL(chain.Connect({txs[2], txs[3], refund}), "");
    chain.Check("8f2f93c1e37d0ebd12c389ecf5db0c892feceb6b142420c5cc83000fee970fb8", "a3811715518ecbe21a03f55db0cfefd50aa8bfb4f8adbf52e44546ac4759be72", 1);
    BOOST_CHECK(chain.paid[0] == (CTxOut{COIN, pay}));
    const uint256 root2{chain.Shielded().Tree().Root()};
    BOOST_CHECK_EQUAL(chain.Shielded().Pool(), 6000);
    BOOST_CHECK_EQUAL(chain.Shielded().AnchorCount(), 3U);

    // A block with nothing shielded leaves no anchor.
    BOOST_REQUIRE_EQUAL(chain.Connect({}), "");
    chain.Check("29dd9ee1819baba6c902c44cfb969fd8f80457319086cbb446f5f0e7fc8e8296", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);
    BOOST_CHECK_EQUAL(chain.Shielded().AnchorCount(), 3U);

    // A spends again: its nullifier is spent.
    BOOST_CHECK_EQUAL(chain.Connect({txs[2]}), "bad-zs-double-spend");

    // Block 4: B spent at the older root, A' at the newer.
    BOOST_CHECK(chain.Shielded().IsAnchor(root1) && chain.Shielded().IsAnchor(root2));
    BOOST_REQUIRE_EQUAL(chain.Connect({txs[4], txs[5]}), "");
    chain.Check("ce87ff64199ca80a07754716f4a01a9dbf9c0537aa79a0fe8c854cefc8a5e964", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);
    BOOST_CHECK_EQUAL(chain.Shielded().Pool(), 6000);
    BOOST_CHECK_EQUAL(chain.Shielded().AnchorCount(), 4U);
    BOOST_CHECK_EQUAL(chain.Shielded().NullifierCount(), 12U);
    BOOST_CHECK_EQUAL(chain.Shielded().Tree().Size(), 12U);
}

BOOST_AUTO_TEST_SUITE_END()
