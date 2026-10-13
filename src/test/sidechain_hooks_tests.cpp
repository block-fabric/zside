// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <blockencodings.h>
#include <chain.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <interfaces/mining.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sidechain/hooks.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <txmempool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The generic extension points of the template (sidechain/hooks.h): what they do by default, which
// must be the template's behaviour exactly, and what they do when a sidechain switches them on.

namespace {

/** Hooks of a sidechain with extended blocks of its own kind, which take no peg actions. */
struct TestHooks : sidechain::ChainHooks {
    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override { return header.IsExtended() ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM; }
    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader&) const override { return (prev ? prev->nChainWork : arith_uint256{0}) + 7; }
    bool RefundsOnlyInPegSteps() const override { return true; }
    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const override
    {
        if (!block.IsExtended()) return ChainHooks::GetPegStep(block, mainchain, step, reject_reason);
        step.reset();
        return true;
    }
};

/** Installs hooks for the life of the object; the template's again after. */
struct InstallHooks {
    explicit InstallHooks(std::unique_ptr<sidechain::ChainHooks> hooks) { sidechain::SetHooks(std::move(hooks)); }
    ~InstallHooks() { sidechain::SetHooks(nullptr); }
};

CBlockHeader Header(bool extension_bit)
{
    CBlockHeader header;
    header.nVersion = 0x20000000 | (extension_bit ? BLOCK_EXTENSION_BIT : 0);
    header.hashPrevBlock = uint256{1};
    header.hashMerkleRoot = uint256{2};
    header.nTime = 1700000000;
    header.nBits = 0x207fffff;
    header.nNonce = 3;
    header.ext.hashSections = uint256{4};
    header.ext.sig.fill(5);
    return header;
}

/** SHA256d of the 80 bytes of Bitcoin's header, and of what follows them in the hash. */
uint256 HashOf(const CBlockHeader& h, const std::optional<uint256>& sections = std::nullopt)
{
    HashWriter hasher{};
    hasher << h.nVersion << h.hashPrevBlock << h.hashMerkleRoot << h.nTime << h.nBits << h.nNonce;
    if (sections) hasher << *sections;
    return hasher.GetHash();
}

CBlock CoinbaseBlock(bool extension_bit, std::vector<CTxOut> extra_outputs = {})
{
    CBlock block{Header(extension_bit)};
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << 2 << OP_0;
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    coinbase.vout.insert(coinbase.vout.end(), extra_outputs.begin(), extra_outputs.end());
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

void AppendMain(sidechain::Mainchain& mainchain, int64_t time, std::optional<uint256> bmm = std::nullopt)
{
    sidechain::MainBlock block;
    block.hash = uint256{static_cast<uint8_t>(0x80 + mainchain.Height() + 1)};
    block.prev_hash = mainchain.TipHash();
    block.time = time;
    block.bmm = bmm;
    BOOST_REQUIRE(mainchain.Append(block));
}

/** A transaction with these outputs, spending some coin. */
CTransaction Tx(std::vector<CTxOut> outputs, uint8_t salt = 0x77)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{salt}), 1});
    tx.vout = std::move(outputs);
    return CTransaction{tx};
}

CTxOut WithdrawalOut(CAmount amount)
{
    const uint160 keyhash{*uint160::FromHex("0101010101010101010101010101010101010101")};
    return CTxOut{amount, sidechain::WithdrawalScript(1000, keyhash, GetScriptForDestination(WitnessV0KeyHash{keyhash}))};
}

/**
 * Hooks with peg actions of their own: a block that takes the peg actions owes 7 satoshis to the
 * script of its coinbase's first output, and every block notes what its coinbase paid out, under a
 * table byte of its own.
 */
const sidechain::Table<uint32_t, int64_t> MINTED_TABLE{0xF0};

struct OwedHooks : sidechain::ChainHooks {
    mutable std::vector<CBlock> seen;
    bool BeforePegStep(sidechain::State&, const CBlock& block, int, const sidechain::PegStep&, const Consensus::SidechainParams&,
                       std::vector<CTxOut>& owed_tx, std::string&) const override
    {
        seen.push_back(block);
        owed_tx.emplace_back(7, block.vtx.at(0)->vout.at(0).scriptPubKey);
        return true;
    }
    bool AfterBlock(sidechain::State& state, const CBlock&, int height, const std::optional<sidechain::PegStep>&, CAmount minted,
                    const Consensus::SidechainParams&, std::string&) const override
    {
        MINTED_TABLE.Put(state.Writable(), static_cast<uint32_t>(height), minted);
        return true;
    }
};

/**
 * Hooks with transaction rules of their own: an output with the script OP_3 breaks them; one with
 * OP_4 is noted under a table byte of their own (by its value) and owes its value back to OP_TRUE.
 */
const sidechain::Table<uint32_t, int64_t> NOTED_TABLE{0xF1};

struct TxRulesHooks : sidechain::ChainHooks {
    bool ApplyTx(sidechain::State& state, const CTransaction& tx, int height, bool, std::vector<CTxOut>& payouts, std::string& reject_reason) const override
    {
        for (const CTxOut& out : tx.vout) {
            if (out.scriptPubKey == (CScript() << OP_4)) {
                NOTED_TABLE.Put(state.Writable(), static_cast<uint32_t>(out.nValue), height);
                payouts.emplace_back(out.nValue, CScript() << OP_TRUE);
            }
            if (out.scriptPubKey == (CScript() << OP_3)) {
                reject_reason = "bad-test-output";
                return false;
            }
        }
        return true;
    }
};

/** Hooks that punish peers for headers of or on blocks that failed against the record, as Bitcoin. */
struct NoPardonHooks : sidechain::ChainHooks {
    bool PardonRecordFailures() const override { return false; }
};

/**
 * Hooks that let every waiting header be tried, all with the same key: they are then tried in the order
 * of their hashes, a child before its parent as often as not.
 */
struct SameKeyHooks : sidechain::ChainHooks {
    std::optional<int64_t> WaitingReady(const CBlockHeader&, const sidechain::Mainchain&) const override { return 0; }
};

/** Hooks that let blocks be at most a minute ahead of the clock. */
struct NearTimeHooks : sidechain::ChainHooks {
    int64_t MaxFutureBlockTime() const override { return 60; }
};

/**
 * Hooks with a chain order of their own that reads something that can change, as a record of the
 * mainchain does: each block adds 1 and `bonus` to the key of its parent.
 */
struct BonusHooks : sidechain::ChainHooks {
    mutable int64_t bonus{0};
    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader&) const override
    {
        return (prev ? prev->nChainWork : arith_uint256{0}) + arith_uint256{static_cast<uint64_t>(1 + bonus)};
    }
};

/**
 * Hooks whose extended blocks are of the sidechain's own kind, take no peg actions, and are valid if
 * their "signature" (which the block hash does not cover) is that of the test's signer: all fives.
 */
struct SignedHooks : sidechain::ChainHooks {
    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override { return header.IsExtended() ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM; }
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex&, const ChainstateManager&, BlockValidationState& state) const override
    {
        std::array<unsigned char, 64> signed_sig;
        signed_sig.fill(5);
        if (header.ext.sig != signed_sig) return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "bad-test-sig");
        return true;
    }
    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const override
    {
        if (!block.IsExtended()) return ChainHooks::GetPegStep(block, mainchain, step, reject_reason);
        step.reset();
        return true;
    }
};

/**
 * SignedHooks whose signer may sign a block twice: all sevens is another valid signature of a block
 * signed with all fives, all eights one the node cannot check yet.
 */
struct TwiceSignedHooks : SignedHooks {
    sidechain::OtherExtension CheckOtherExtension(const CBlockHeader& header, const CBlockIndex&) const override
    {
        std::array<unsigned char, 64> other, unknown;
        other.fill(7);
        unknown.fill(8);
        if (header.ext.sig == other) return sidechain::OtherExtension::EQUIVALENT;
        if (header.ext.sig == unknown) return sidechain::OtherExtension::UNKNOWN;
        return sidechain::OtherExtension::INVALID;
    }
};

/** Hooks whose merged mined blocks carry an output of their own, and that note what they were asked for. */
struct OwnOutputHooks : sidechain::ChainHooks {
    mutable std::vector<uint256> asked;
    std::vector<CTxOut> MergeMinedOutputs(const sidechain::StoreView&, const CBlockIndex& prev, int64_t) const override
    {
        asked.push_back(prev.GetBlockHash());
        return {CTxOut{0, CScript() << OP_RETURN << std::vector<unsigned char>{0xDD, 'O', 'W', 'N'}}};
    }
};

/** Hooks whose extended blocks take the peg actions of the step the test sets. */
struct StepHooks : sidechain::ChainHooks {
    mutable sidechain::PegStep step;
    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override { return header.IsExtended() ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM; }
    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& out, std::string& reject_reason) const override
    {
        if (!block.IsExtended()) return ChainHooks::GetPegStep(block, mainchain, out, reject_reason);
        out = step;
        return true;
    }
};

/** A chain that is a sidechain in slot 3, with a record of its mainchain the test writes. */
struct SidechainSetup : public TestingSetup {
    SidechainSetup() : TestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=3"}}} {}
    sidechain::Mainchain& Record() { return *Assert(m_node.chainman->m_mainchain); }

    /** A block on the tip, with this time (the clock's if none), its proof of work done. */
    CBlock Build(std::optional<int64_t> time = std::nullopt)
    {
        auto block_template{interfaces::MakeMining(m_node)->createNewBlock({.use_mempool = false, .coinbase_output_script = CScript() << OP_TRUE, .test_block_validity = false}, /*cooldown=*/false)};
        BOOST_REQUIRE(block_template);
        CBlock block{block_template->getBlock()};
        if (time) block.nTime = static_cast<uint32_t>(*time);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, m_node.chainman->GetConsensus())) ++block.nNonce;
        return block;
    }
    /** Have the next mainchain block commit to `hash`. */
    void Commit(const uint256& hash)
    {
        sidechain::MainBlock main;
        main.hash = uint256{static_cast<uint8_t>(0x40 + Record().Height() + 1)};
        main.prev_hash = Record().TipHash();
        main.bmm = hash;
        BOOST_REQUIRE(Record().Append(main));
    }
    /** Commit to a block and give it to the node. */
    void Take(const CBlock& block)
    {
        Commit(block.GetHash());
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr));
        BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()) == block.GetHash());
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_hooks_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(template_defaults)
{
    // Without hooks of a sidechain's own, every hook does what the template does.
    const sidechain::ChainHooks& hooks{sidechain::Hooks()};
    BOOST_CHECK(!hooks.BlockExtensions());
    BOOST_CHECK(!BlockExtensionsEnabled());
    BOOST_CHECK(!hooks.CustomChainOrder());
    BOOST_CHECK(!hooks.RefundsOnlyInPegSteps());
    // Headers of or on blocks that failed against the record do not get peers punished.
    BOOST_CHECK(hooks.PardonRecordFailures());
    // Another extension than the header's is a mutated block; merged mined blocks carry nothing of a sidechain's own.
    BOOST_CHECK(hooks.CheckOtherExtension(Header(true), CBlockIndex{Header(true)}) == sidechain::OtherExtension::INVALID);

    // Every block is merged mined, the extension bit included: it means nothing.
    const CBlockHeader plain{Header(false)}, flagged{Header(true)};
    BOOST_CHECK(hooks.Kind(plain) == sidechain::BlockKind::BMM);
    BOOST_CHECK(hooks.Kind(flagged) == sidechain::BlockKind::BMM);

    // Blocks may be two hours ahead of the clock, as Bitcoin's.
    BOOST_CHECK_EQUAL(hooks.MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);

    // Headers wait only for a commitment.
    BOOST_CHECK(hooks.IsWaitReason("bmm-unknown"));
    BOOST_CHECK(!hooks.IsWaitReason("bmm-prev-unknown"));
    BOOST_CHECK(!hooks.IsWaitReason("bad-pos-sig"));

    // The chain with the most work: that of the parent plus the header's.
    CBlockIndex prev{plain};
    prev.nChainWork = 5;
    BOOST_CHECK(hooks.ChainWork(&prev, plain) == arith_uint256{5} + GetBlockProof(plain));
    BOOST_CHECK(hooks.ChainWork(nullptr, plain) == GetBlockProof(plain));

    // A block needs a commitment to itself, and only it rests on that commitment.
    const uint256 hash{plain.GetHash()};
    CBlockIndex index{plain};
    index.phashBlock = &hash;
    BOOST_CHECK(hooks.CommitmentOf(index) == hash);
    BOOST_CHECK(hooks.BlocksCommittedBy(uint256{9}) == std::vector<uint256>{uint256{9}});

    // No kinds of blocks of its own, no rules of its own, no messages of its own.
    BlockValidationState state;
    BOOST_CHECK(!hooks.CheckHeader(plain, prev, *m_node.chainman, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-block-kind");
    BlockValidationState block_state;
    BOOST_CHECK(hooks.CheckBlock(CoinbaseBlock(false), block_state));
    BOOST_CHECK(block_state.IsValid());
    DataStream recv{};
    std::string misbehaving;
    BOOST_CHECK(!hooks.ProcessMessage(1, "ticket", recv, misbehaving));
    BOOST_CHECK(misbehaving.empty());

    // The sections hash is a tagged hash, of the bytes as they are.
    const std::vector<unsigned char> sections{1, 2, 3};
    BOOST_CHECK(hooks.SectionsHash(sections) == (TaggedHash("sidechain/sections") << std::span<const unsigned char>{sections}).GetSHA256());
    BOOST_CHECK(hooks.SectionsHash(sections) != hooks.SectionsHash(std::vector<unsigned char>{1, 2}));

    // Peg actions in every block, as committed: up to the mainchain block before the commitment,
    // with the parent as the bundle nonce, and no limit on the withdrawals a bundle takes.
    const CBlock block{CoinbaseBlock(false)};
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    std::optional<sidechain::PegStep> step;
    std::string reason;
    BOOST_CHECK(!hooks.GetPegStep(block, mainchain, step, reason));
    BOOST_CHECK_EQUAL(reason, "bmm-unknown");
    BOOST_CHECK(!hooks.WaitingReady(block, mainchain));
    AppendMain(mainchain, 101, block.GetHash());
    BOOST_CHECK(hooks.GetPegStep(block, mainchain, step, reason));
    BOOST_REQUIRE(step);
    BOOST_CHECK(*step == (sidechain::PegStep{.main_height = 0, .nonce = block.hashPrevBlock, .max_withdrawal_height = std::nullopt, .peg_height = std::nullopt}));
    BOOST_CHECK(hooks.WaitingReady(block, mainchain) == std::optional<int64_t>{1});

    // The sidechain's own peg actions and notes: none.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State side{store};
    std::vector<CTxOut> owed_tx;
    BOOST_CHECK(hooks.BeforePegStep(side, block, 1, *step, Params().GetConsensus().sidechain, owed_tx, reason));
    BOOST_CHECK(owed_tx.empty());
    BOOST_CHECK(hooks.AfterBlock(side, block, 1, step, /*minted=*/0, Params().GetConsensus().sidechain, reason));
    BOOST_CHECK(hooks.ApplyTx(side, Tx({CTxOut{5, CScript() << OP_3}}), 1, /*peg_step=*/true, owed_tx, reason));
    BOOST_CHECK(owed_tx.empty());
    BOOST_CHECK(store.Changes().empty());

    // Proof of work for every header.
    CBlockHeader mined{plain};
    while (!CheckProofOfWork(mined.GetHash(), mined.nBits, Params().GetConsensus())) ++mined.nNonce;
    BOOST_CHECK(sidechain::CheckHeaderWork(mined, Params().GetConsensus()));
    CBlockHeader unmined{plain};
    unmined.nBits = 0x1d00ffff;
    BOOST_CHECK(!sidechain::CheckHeaderWork(unmined, Params().GetConsensus()));
}

BOOST_AUTO_TEST_CASE(extensions_off)
{
    // Off, the extension bit changes nothing: Bitcoin's 80 bytes, Bitcoin's hash, no sections.
    const CBlockHeader header{Header(true)};
    BOOST_CHECK(!header.IsExtended());
    DataStream ss{};
    ss << header;
    BOOST_CHECK_EQUAL(ss.size(), 80U);
    BOOST_CHECK(header.GetHash() == HashOf(header));
    CBlockHeader no_work{header};
    no_work.nBits = 0;
    BOOST_CHECK(no_work.IsNull());

    CBlock block{CoinbaseBlock(true)};
    CBlock without{block};
    block.vSections = {1, 2, 3};
    BOOST_CHECK_EQUAL(GetSerializeSize(TX_WITH_WITNESS(block)), GetSerializeSize(TX_WITH_WITNESS(without)));
    BOOST_CHECK(sidechain::CheckSections(block));

    // The block index stores what it always did.
    CBlockIndex index{header};
    BOOST_CHECK(!index.m_ext);
    DataStream disk{};
    disk << CDiskBlockIndex{&index};
    CDiskBlockIndex read;
    disk >> read;
    BOOST_CHECK(disk.empty());
    BOOST_CHECK(!read.m_ext);
}

BOOST_AUTO_TEST_CASE(extensions_on)
{
    InstallHooks installed{std::make_unique<TestHooks>()};
    BOOST_CHECK(BlockExtensionsEnabled());

    // An extended header: the 80 bytes and the extension; the hash covers hashSections, not the signature.
    CBlockHeader header{Header(true)};
    BOOST_CHECK(header.IsExtended());
    DataStream ss{};
    ss << header;
    BOOST_CHECK_EQUAL(ss.size(), 80U + 32U + 64U);
    CBlockHeader read;
    ss >> read;
    BOOST_CHECK(read.ext == header.ext);
    BOOST_CHECK(read.GetHash() == header.GetHash());
    BOOST_CHECK(header.GetHash() == HashOf(header, header.ext.hashSections));
    CBlockHeader resigned{header};
    resigned.ext.sig.fill(6);
    BOOST_CHECK(resigned.GetHash() == header.GetHash());
    CBlockHeader other_sections{header};
    other_sections.ext.hashSections = uint256{7};
    BOOST_CHECK(other_sections.GetHash() != header.GetHash());
    // Without proof of work, a header all the same.
    CBlockHeader no_work{header};
    no_work.nBits = 0;
    BOOST_CHECK(!no_work.IsNull());
    BOOST_CHECK(sidechain::CheckHeaderWork(no_work, Params().GetConsensus()));

    // A header without the bit is Bitcoin's, as before.
    const CBlockHeader plain{Header(false)};
    DataStream plain_ss{};
    plain_ss << plain;
    BOOST_CHECK_EQUAL(plain_ss.size(), 80U);
    BOOST_CHECK(plain.GetHash() == HashOf(plain));

    // The sections of an extended block go after its transactions, and the header commits to them.
    CBlock block{CoinbaseBlock(true)};
    block.vSections = {1, 2, 3};
    block.ext.hashSections = sidechain::Hooks().SectionsHash(block.vSections);
    BOOST_CHECK(sidechain::CheckSections(block));
    DataStream block_ss{};
    block_ss << TX_WITH_WITNESS(block);
    CBlock block_read;
    block_ss >> TX_WITH_WITNESS(block_read);
    BOOST_CHECK(block_read.vSections == block.vSections);
    BOOST_CHECK(block_read.GetHash() == block.GetHash());
    // No sections: a null hash.
    CBlock empty{CoinbaseBlock(true)};
    empty.ext.hashSections.SetNull();
    BOOST_CHECK(sidechain::CheckSections(empty));
    empty.ext.hashSections = uint256{1};
    BOOST_CHECK(!sidechain::CheckSections(empty));

    // Sections other than those committed to: a mutated block, never cached as invalid.
    CBlock mutated{block};
    mutated.vSections = {1, 2, 4};
    BOOST_CHECK(IsBlockMutated(mutated, /*check_witness_root=*/false));
    BOOST_CHECK(!IsBlockMutated(block, /*check_witness_root=*/false));
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(mutated, state, Params().GetConsensus(), /*fCheckPOW=*/true, /*fCheckMerkleRoot=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-sections-hash");
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BlockValidationState ok_state;
    BOOST_CHECK_MESSAGE(CheckBlock(block, ok_state, Params().GetConsensus(), /*fCheckPOW=*/true, /*fCheckMerkleRoot=*/true), ok_state.ToString());

    // The block index keeps the extension, and gives the header back as it was.
    CBlockIndex index{block};
    BOOST_REQUIRE(index.m_ext);
    BOOST_CHECK(*index.m_ext == block.ext);
    BOOST_CHECK(index.GetBlockHeader().ext == block.ext);
    DataStream disk{};
    disk << CDiskBlockIndex{&index};
    CDiskBlockIndex disk_read;
    disk >> disk_read;
    BOOST_CHECK(disk.empty());
    BOOST_REQUIRE(disk_read.m_ext);
    BOOST_CHECK(*disk_read.m_ext == block.ext);
    CBlockHeader unlinked{static_cast<const CBlockHeader&>(block)};
    unlinked.hashPrevBlock.SetNull();
    BOOST_CHECK(disk_read.ConstructBlockHash() == unlinked.GetHash());

    // Compact blocks carry the sections whole.
    const CBlockHeaderAndShortTxIDs compact{block, /*nonce=*/42};
    DataStream compact_ss{};
    compact_ss << compact;
    CBlockHeaderAndShortTxIDs compact_read;
    compact_ss >> compact_read;
    BOOST_CHECK(compact_read.sections == block.vSections);
    PartiallyDownloadedBlock partial{m_node.mempool.get()};
    BOOST_REQUIRE(partial.InitData(compact_read, {}) == READ_STATUS_OK);
    CBlock filled;
    BOOST_REQUIRE(partial.FillBlock(filled, {}, /*segwit_active=*/true) == READ_STATUS_OK);
    BOOST_CHECK(filled.vSections == block.vSections);
    BOOST_CHECK(filled.GetHash() == block.GetHash());

    // The chain order of the sidechain's.
    BOOST_CHECK(sidechain::Hooks().CustomChainOrder());
    BOOST_CHECK(sidechain::Hooks().ChainWork(nullptr, header) == arith_uint256{7});
}

BOOST_AUTO_TEST_CASE(hooks_reinstalled)
{
    // Installing hooks gives back those before; nullptr puts the template's back, extended blocks off.
    BOOST_CHECK(!sidechain::SetHooks(std::make_unique<TestHooks>()));
    BOOST_CHECK(BlockExtensionsEnabled());
    const auto before{sidechain::SetHooks(nullptr)};
    BOOST_CHECK(before);
    BOOST_CHECK(!BlockExtensionsEnabled());
    BOOST_CHECK(!sidechain::Hooks().CustomChainOrder());
}

BOOST_AUTO_TEST_CASE(mainchain_time_height)
{
    sidechain::Mainchain mainchain;
    BOOST_CHECK(!mainchain.TimeHeight(1000));
    BOOST_CHECK(!mainchain.HashAt(0));
    AppendMain(mainchain, 100);
    AppendMain(mainchain, 100);
    AppendMain(mainchain, 150);
    AppendMain(mainchain, 200);
    // The highest block with a median time at or below the time.
    BOOST_CHECK(!mainchain.TimeHeight(99));
    BOOST_CHECK(mainchain.TimeHeight(100) == std::optional<int>{1});
    BOOST_CHECK(mainchain.TimeHeight(149) == std::optional<int>{1});
    BOOST_CHECK(mainchain.TimeHeight(150) == std::optional<int>{2});
    BOOST_CHECK(mainchain.TimeHeight(199) == std::optional<int>{2});
    BOOST_CHECK(mainchain.TimeHeight(1000) == std::optional<int>{3});
    BOOST_CHECK(mainchain.HashAt(2) == mainchain.GetBlock(2)->hash);
    BOOST_CHECK(mainchain.HashAt(3) == mainchain.TipHash());
    BOOST_CHECK(!mainchain.HashAt(4));
    BOOST_CHECK(!mainchain.HashAt(-1));
    mainchain.Truncate(1);
    BOOST_CHECK(mainchain.TimeHeight(1000) == std::optional<int>{1});
}

BOOST_AUTO_TEST_CASE(bundle_withdrawal_height)
{
    // A bundle may be limited to withdrawals made at or below a height; by default it takes any.
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(Tx({WithdrawalOut(COIN)}, 1), 1, params, payouts, reason), reason);
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(Tx({WithdrawalOut(2 * COIN)}, 2), 5, params, payouts, reason), reason);
    std::vector<COutPoint> all, low, none;
    BOOST_REQUIRE(state.NextBundle(10, uint256{0xa}, params, &all));
    BOOST_CHECK_EQUAL(all.size(), 2U);
    BOOST_REQUIRE(state.NextBundle(10, uint256{0xa}, params, &low, /*max_withdrawal_height=*/3));
    BOOST_REQUIRE_EQUAL(low.size(), 1U);
    BOOST_CHECK_EQUAL(state.GetWithdrawal(low[0])->height, 1);
    BOOST_CHECK(!state.NextBundle(10, uint256{0xa}, params, &none, /*max_withdrawal_height=*/0));
    // Started with the limit, the bundle has the withdrawals of the limit.
    const auto bundle{state.NextBundle(10, uint256{0xa}, params, nullptr, 3)};
    BOOST_CHECK(!state.StartBundle(bundle->GetHash().ToUint256(), 10, uint256{0xa}, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-sc-bundle-hash");
    BOOST_REQUIRE_MESSAGE(state.StartBundle(bundle->GetHash().ToUint256(), 10, uint256{0xa}, params, reason, 3), reason);
    BOOST_CHECK(state.Bundle()->withdrawals == low);
}

BOOST_AUTO_TEST_CASE(payouts_without_peg_step)
{
    // A block without the peg actions pays nothing: what is owed waits, in order, for one that has them.
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    const std::vector<CTxOut> owed{{1, CScript() << OP_TRUE}}, owed_tx{{2, CScript() << OP_TRUE}};
    BOOST_CHECK(state.TakePayouts(owed, owed_tx, 0).empty());
    BOOST_CHECK(state.Queue() == owed);
    BOOST_CHECK(state.TxQueue() == owed_tx);
    const auto paid{state.TakePayouts({}, {})};
    BOOST_REQUIRE_EQUAL(paid.size(), 2U);
    BOOST_CHECK(paid[0] == owed[0]);
    BOOST_CHECK(paid[1] == owed_tx[0]);
    BOOST_CHECK(state.Queue().empty() && state.TxQueue().empty());
}

BOOST_AUTO_TEST_CASE(block_without_peg_step)
{
    InstallHooks installed{std::make_unique<TestHooks>()};
    const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    sidechain::EmptyStore empty;
    CAmount minted{-1};
    std::string reason;

    // A block of the sidechain's own kind that takes no peg actions: it acts on nothing the
    // mainchain did, and needs no commitment.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK_MESSAGE(state.ConnectBlock(CoinbaseBlock(true), 1, params, mainchain, minted, reason), reason);
        BOOST_CHECK_EQUAL(minted, 0);
        BOOST_CHECK_EQUAL(state.MainHeight(), -1);
    }
    // A block of the template's kind still needs its commitment.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK(!state.ConnectBlock(CoinbaseBlock(false), 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bmm-unknown");
    }
    // It cannot start a bundle.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK(!state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(uint256{1})}}), 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-bundle-not-peg");
    }
    // Nor take a refund request, when refunds wait for the peg actions.
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256{3}), 0};
    request.signature.assign(65, 1);
    const CTransaction refund{Tx({CTxOut{0, sidechain::RefundScript(request)}})};
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        CBlock block{CoinbaseBlock(true)};
        block.vtx.push_back(MakeTransactionRef(refund));
        BOOST_CHECK(!state.ConnectBlock(block, 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-refund-not-peg");
        std::vector<CTxOut> payouts;
        BOOST_CHECK(!state.ApplyTx(refund, 1, params, payouts, reason, /*main_pending=*/false, /*peg_step=*/false));
        BOOST_CHECK_EQUAL(reason, "bad-sc-refund-not-peg");
        // With the peg actions it is checked as any refund (and this one refunds nothing).
        BOOST_CHECK(!state.ApplyTx(refund, 1, params, payouts, reason, /*main_pending=*/false, /*peg_step=*/true));
        BOOST_CHECK_EQUAL(reason, "bad-sc-refund-unknown");
    }
    // Withdrawals are made in it all the same.
    {
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        CBlock block{CoinbaseBlock(true)};
        block.vtx.push_back(MakeTransactionRef(Tx({WithdrawalOut(COIN)})));
        BOOST_CHECK_MESSAGE(state.ConnectBlock(block, 1, params, mainchain, minted, reason), reason);
        size_t withdrawals{0};
        state.ForEachWithdrawal([&](const sidechain::Withdrawal&) { ++withdrawals; return true; });
        BOOST_CHECK_EQUAL(withdrawals, 1U);
    }
}

BOOST_AUTO_TEST_CASE(template_refunds_anywhere)
{
    // In the template a refund request is checked as before whatever the block: the switch is off.
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256{3}), 0};
    request.signature.assign(65, 1);
    std::vector<CTxOut> payouts;
    std::string reason;
    BOOST_CHECK(!state.ApplyTx(Tx({CTxOut{0, sidechain::RefundScript(request)}}), 1, params, payouts, reason, /*main_pending=*/false, /*peg_step=*/false));
    BOOST_CHECK_EQUAL(reason, "bad-sc-refund-unknown");
}

BOOST_AUTO_TEST_CASE(own_tx_rules)
{
    // The rules of a sidechain's own for a transaction come after the template's, all or nothing: a
    // transaction that breaks them leaves neither its withdrawal nor anything they wrote, nor payouts.
    InstallHooks installed{std::make_unique<TxRulesHooks>()};
    const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    std::vector<CTxOut> payouts{CTxOut{1, CScript() << OP_2}};
    std::string reason;
    const CTransaction bad{Tx({WithdrawalOut(params.min_withdrawal + 1000), CTxOut{9, CScript() << OP_4}, CTxOut{0, CScript() << OP_3}})};
    BOOST_CHECK(!state.ApplyTx(bad, 5, params, payouts, reason));
    BOOST_CHECK_EQUAL(reason, "bad-test-output");
    BOOST_CHECK(store.Changes().empty());
    BOOST_CHECK_EQUAL(payouts.size(), 1U);
    // One that keeps them: its withdrawal, what they wrote, and what they owe.
    const CTransaction good{Tx({WithdrawalOut(params.min_withdrawal + 1000), CTxOut{9, CScript() << OP_4}}, 0x78)};
    BOOST_CHECK_MESSAGE(state.ApplyTx(good, 5, params, payouts, reason), reason);
    BOOST_CHECK(state.GetWithdrawal(COutPoint{good.GetHash(), 0}));
    BOOST_CHECK(NOTED_TABLE.Get(state.View(), 9) == std::optional<int64_t>{5});
    BOOST_REQUIRE_EQUAL(payouts.size(), 2U);
    BOOST_CHECK(payouts[1] == CTxOut(9, CScript() << OP_TRUE));
}

BOOST_AUTO_TEST_CASE(own_peg_actions)
{
    // What a sidechain's own peg actions owe is paid by the coinbase, as refunds are; the notes it
    // takes of a block (AfterBlock) see what the coinbase paid out, and go in the state.
    InstallHooks installed{std::make_unique<OwedHooks>()};
    const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    sidechain::EmptyStore empty;
    CAmount minted{-1};
    std::string reason;
    CBlock block{CoinbaseBlock(false)};
    AppendMain(mainchain, 101, block.GetHash());
    {
        // The coinbase does not pay it.
        sidechain::StoreOverlay store{empty, /*journal=*/true};
        sidechain::State state{store};
        BOOST_CHECK(!state.ConnectBlock(block, 1, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-payouts-missing");
    }
    CBlock paying{CoinbaseBlock(false, {CTxOut{7, CScript() << OP_TRUE}})};
    AppendMain(mainchain, 102, paying.GetHash());
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    BOOST_CHECK_MESSAGE(state.ConnectBlock(paying, 1, params, mainchain, minted, reason), reason);
    BOOST_CHECK_EQUAL(minted, 7);
    BOOST_CHECK(MINTED_TABLE.Get(state.View(), 1) == std::optional<int64_t>{7});
    // Taken back with the block's undo data.
    store.Revert(store.TakeUndo());
    BOOST_CHECK(!MINTED_TABLE.Get(store, 1));
}

BOOST_FIXTURE_TEST_CASE(assembly_takes_own_peg_actions, SidechainSetup)
{
    // Block assembly calls the sidechain's own peg actions too, with the block as far as it is known
    // (parent, sections, a coinbase paying to the coinbase script), and its coinbase pays what they owe.
    auto owned{std::make_unique<OwedHooks>()};
    const OwedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock main;
    main.hash = uint256{0x51};
    BOOST_REQUIRE(Record().Append(main));
    const CScript script{CScript() << OP_2};
    node::BlockCreateOptions options;
    options.coinbase_output_script = script;
    options.test_block_validity = false;
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    const uint256 tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};

    // The template's block: the peg actions as of the next mainchain block.
    {
        const auto block_template{node::BlockAssembler{chainstate, m_node.mempool.get(), options}.CreateNewBlock()};
        BOOST_REQUIRE_EQUAL(hooks.seen.size(), 1U);
        BOOST_CHECK(hooks.seen[0].hashPrevBlock == tip);
        BOOST_CHECK(hooks.seen[0].vtx.at(0)->vout.at(0) == CTxOut(0, script));
        BOOST_CHECK(hooks.seen[0].vSections.empty());
        const CTransaction& coinbase{*block_template->block.vtx.at(0)};
        BOOST_REQUIRE_GE(coinbase.vout.size(), 2U);
        BOOST_CHECK(coinbase.vout[1] == CTxOut(7, script));
    }
    // A block with the peg actions of a given step, and sections.
    node::SidechainAssembly assembly;
    assembly.peg = node::SidechainAssembly::Peg::STEP;
    assembly.step = sidechain::PegStep{.main_height = 0, .nonce = tip, .max_withdrawal_height = std::nullopt, .peg_height = std::nullopt};
    assembly.sections = {1, 2, 3};
    {
        const auto block_template{node::BlockAssembler{chainstate, m_node.mempool.get(), options, assembly}.CreateNewBlock()};
        BOOST_REQUIRE_EQUAL(hooks.seen.size(), 2U);
        BOOST_CHECK(hooks.seen[1].vSections == assembly.sections);
        BOOST_CHECK(block_template->block.vtx.at(0)->vout.at(1) == CTxOut(7, script));
    }
    // A block without the peg actions: none of the sidechain's own either, and nothing paid.
    assembly.peg = node::SidechainAssembly::Peg::NONE;
    {
        const auto block_template{node::BlockAssembler{chainstate, m_node.mempool.get(), options, assembly}.CreateNewBlock()};
        BOOST_CHECK_EQUAL(hooks.seen.size(), 2U);
        for (const CTxOut& out : block_template->block.vtx.at(0)->vout) BOOST_CHECK(out != CTxOut(7, script));
    }
}

BOOST_FIXTURE_TEST_CASE(future_block_time, SidechainSetup)
{
    // A header an hour and a half ahead of the clock: within Bitcoin's two hours, the template takes it.
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    const int64_t ahead{GetTime() + 90 * 60};
    {
        const CBlock block{Build(ahead)};
        Commit(block.GetHash());
        BlockValidationState state;
        BOOST_CHECK_MESSAGE(m_node.chainman->ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state), state.ToString());
    }
    // A sidechain that allows a minute turns it away, as a header that may come later; a block
    // half a minute ahead it takes.
    InstallHooks installed{std::make_unique<NearTimeHooks>()};
    const CBlock block{Build(GetTime() + 90)};
    Commit(block.GetHash());
    BlockValidationState state;
    BOOST_CHECK(!m_node.chainman->ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "time-too-new");
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE);
    const CBlock near{Build(GetTime() + 30)};
    Commit(near.GetHash());
    BlockValidationState near_state;
    BOOST_CHECK_MESSAGE(m_node.chainman->ProcessNewBlockHeaders({{near}}, /*min_pow_checked=*/true, near_state), near_state.ToString());
}

BOOST_FIXTURE_TEST_CASE(chain_work_recomputed, SidechainSetup)
{
    // A chain order that reads the record is computed anew for a block, and all above it, when the
    // record takes back its failure (its commitment came back, perhaps at another height).
    auto owned{std::make_unique<BonusHooks>()};
    const BonusHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const arith_uint256 base{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->nChainWork)};
    const CBlock a{Build()};
    Take(a);
    const CBlock b{Build()};
    Take(b);
    CBlockIndex* index_a{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(a.GetHash()))};
    CBlockIndex* index_b{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(b.GetHash()))};
    BOOST_CHECK(index_a->nChainWork == base + 1);
    BOOST_CHECK(index_b->nChainWork == base + 2);

    // The record drops the commitment to a, and it comes back: what the key reads changed meanwhile.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, index_a, /*by_record=*/true));
    hooks.bonus = 10;
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.ReconsiderRecordFailure(index_a)));
    BOOST_CHECK(index_a->nChainWork == base + 11);
    BOOST_CHECK(index_b->nChainWork == base + 22);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.m_best_header) == index_b);
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) == index_b);

    // With a chain order of its own, the header of the most work may be lower than the tip: the
    // verification progress is guessed all the same (Bitcoin's code assumes otherwise).
    {
        LOCK(::cs_main);
        CBlockIndex* const best{chainman.m_best_header};
        chainman.m_best_header = index_a;
        const double progress{chainman.GuessVerificationProgress(index_b)};
        BOOST_CHECK(progress >= 0.0 && progress <= 1.0);
        chainman.m_best_header = best;
    }

    // With Bitcoin's order the work is the parent's and the header's, whatever was before.
    sidechain::SetHooks(nullptr);
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, index_a, /*by_record=*/true));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.ReconsiderRecordFailure(index_a)));
    BOOST_CHECK(index_a->nChainWork == base + 11);
}

BOOST_FIXTURE_TEST_CASE(block_extension_mismatch, SidechainSetup)
{
    // The block hash does not cover an extended header's signature: a block for a header the node
    // has must carry the extension of that header, or it is a mutated block -- turned away without
    // taking the header for invalid, so that the block with the right one is taken after.
    InstallHooks installed{std::make_unique<SignedHooks>()};
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    CBlock honest{Build()};
    honest.nVersion |= BLOCK_EXTENSION_BIT;
    honest.ext.hashSections.SetNull();
    honest.ext.sig.fill(5);
    BOOST_REQUIRE(honest.IsExtended());
    CBlock swapped{honest};
    swapped.ext.sig.fill(6);
    CBlock zeroed{honest};
    zeroed.ext.sig.fill(0);
    BOOST_REQUIRE(swapped.GetHash() == honest.GetHash() && zeroed.GetHash() == honest.GetHash());

    // Without the header, a block with another signature fails the header's rules, and leaves no index.
    {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(swapped), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-test-sig");
        BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(honest.GetHash())));
    }
    // The header comes first, then blocks with its hash and another signature, or none.
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{honest}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    CBlockIndex* index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index && index->m_ext && *index->m_ext == honest.ext);
    BOOST_CHECK(sidechain::SameExtension(honest, *index));
    BOOST_CHECK(!sidechain::SameExtension(swapped, *index));
    for (const CBlock& bad : {swapped, zeroed}) {
        BlockValidationState state;
        bool new_block{true};
        BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(bad), state, nullptr, true, nullptr, &new_block, true)));
        BOOST_CHECK(!new_block);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-extension");
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        BOOST_CHECK(!chainman.ProcessNewBlock(std::make_shared<const CBlock>(bad), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block));
        BOOST_CHECK(!new_block);
        LOCK(::cs_main);
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK(*index->m_ext == honest.ext);
    }
    // The block with the header's signature is taken, and stored with it.
    bool new_block{false};
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(honest), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block));
    BOOST_CHECK(new_block);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) == index);
    CBlock read;
    BOOST_REQUIRE(chainman.m_blockman.ReadBlock(read, *index));
    BOOST_CHECK(read.ext == honest.ext);
    // Once it is stored, a block with another signature is still a mutated one.
    {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(swapped), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        BOOST_CHECK(WITH_LOCK(::cs_main, return index->IsValid(BLOCK_VALID_SCRIPTS)));
    }

    // A block stored with another signature (as a node could, before this rule) is read with the
    // signature of its header: the one the node checked.
    {
        LOCK(::cs_main);
        const FlatFilePos pos{chainman.m_blockman.WriteBlock(swapped, index->nHeight)};
        BOOST_REQUIRE(!pos.IsNull());
        CBlock raw;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(raw, pos, honest.GetHash()));
        BOOST_CHECK(raw.ext == swapped.ext);
        const uint256 hash{honest.GetHash()};
        CBlockIndex stored{honest};
        stored.phashBlock = &hash;
        stored.nStatus = BLOCK_HAVE_DATA;
        stored.nFile = pos.nFile;
        stored.nDataPos = pos.nPos;
        CBlock fixed;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(fixed, stored));
        BOOST_CHECK(fixed.ext == honest.ext);
        BOOST_CHECK(fixed.vtx.size() == honest.vtx.size() && fixed.GetHash() == hash);
    }

    // Blocks without the extension are not touched by the rule.
    sidechain::SetHooks(nullptr);
    BOOST_CHECK(sidechain::SameExtension(Header(false), CBlockIndex{Header(false)}));
}

/**
 * SignedHooks for a chain whose signatures the node can check only once it connects the chain below
 * (`known`): all nines is a forged signature it takes in a header before it can tell, which all fives
 * replaces once it can.
 */
struct LateCheckedHooks : SignedHooks {
    bool known{false};
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const override
    {
        std::array<unsigned char, 64> forged;
        forged.fill(9);
        if (!known && header.ext.sig == forged) return true;
        return SignedHooks::CheckHeader(header, prev, chainman, state);
    }
    sidechain::OtherExtension CheckOtherExtension(const CBlockHeader& header, const CBlockIndex& index) const override
    {
        if (!known) return sidechain::OtherExtension::UNKNOWN;
        std::array<unsigned char, 64> valid;
        valid.fill(5);
        if (header.ext.sig != valid) return sidechain::OtherExtension::INVALID;
        return index.m_ext && index.m_ext->sig == valid ? sidechain::OtherExtension::EQUIVALENT : sidechain::OtherExtension::REPLACE;
    }
};

BOOST_FIXTURE_TEST_CASE(block_extension_unknown_taken, SidechainSetup)
{
    // A block for a header the node has, with another signature the node cannot check yet (it took the
    // header's, forged, before it could tell; it can once it connects the chain below): the same block
    // but for the signature. Taken, with the header's signature, and the other one kept: when the
    // block is connected, where the node can tell, the valid one replaces the forged one. Turned away,
    // the block would stall a branch that needs it to be connected (a fork whose overtaking block it is).
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3e};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    CBlock honest{Build()};
    honest.nVersion |= BLOCK_EXTENSION_BIT;
    honest.ext.hashSections.SetNull();
    honest.ext.sig.fill(5);
    CBlock forged{honest};
    forged.ext.sig.fill(9);
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{forged}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    CBlockIndex* index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index);
    {
        BlockValidationState state;
        bool new_block{false};
        BOOST_CHECK_MESSAGE(WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(honest), state, nullptr, true, nullptr, &new_block, true, /*source=*/7)), state.ToString());
        BOOST_CHECK(new_block);
        BOOST_CHECK(state.IsValid());
        LOCK(::cs_main);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(*index->m_ext == forged.ext);
        BOOST_CHECK(chainman.m_other_extensions.contains(honest.GetHash()));
    }
    {
        // Stored with the header's (the node serves what it has; what the block hash covers is the same).
        CBlock raw;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(raw, *index));
        BOOST_CHECK(raw.ext == forged.ext);
    }
    // The node can tell now (the chain below connected): the honest signature replaces the forged one
    // as the block is connected.
    hooks.known = true;
    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) == index);
    BOOST_CHECK(*index->m_ext == honest.ext);
    CBlock raw;
    BOOST_REQUIRE(chainman.m_blockman.ReadBlock(raw, *index));
    BOOST_CHECK(raw.ext == honest.ext);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.m_other_extensions.contains(honest.GetHash())));
}

BOOST_FIXTURE_TEST_CASE(header_extension_replaced, SidechainSetup)
{
    // A header the node took with a signature it could not check then (forged), and that comes again
    // with a valid one once the node can tell: the valid one replaces it (CheckOtherExtension REPLACE).
    // Without the hook (INVALID, the template's), the node keeps the one it has.
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3d};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    CBlock honest{Build()};
    honest.nVersion |= BLOCK_EXTENSION_BIT;
    honest.ext.hashSections.SetNull();
    honest.ext.sig.fill(5);
    CBlock forged{honest};
    forged.ext.sig.fill(9);
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{forged}}, /*min_pow_checked=*/true, state), state.ToString());
    const CBlockIndex* index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index);
    BOOST_CHECK(WITH_LOCK(::cs_main, return *index->m_ext == forged.ext));
    hooks.known = true;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{honest}}, /*min_pow_checked=*/true, state), state.ToString());
    BOOST_CHECK(WITH_LOCK(::cs_main, return *index->m_ext == honest.ext));
}

/** Hooks that note where the headers they are shown come from. */
struct SourceHooks : sidechain::ChainHooks {
    mutable std::vector<int64_t> sources;
    mutable int open{0};
    void BeginHeaders(std::span<const CBlockHeader>, int64_t source) const override
    {
        sources.push_back(source);
        ++open;
    }
    void EndHeaders() const override { --open; }
};

BOOST_FIXTURE_TEST_CASE(headers_named_by_source, SidechainSetup)
{
    // The hooks are shown each message's headers, and where they come from: the peer, -1 for none.
    auto owned{std::make_unique<SourceHooks>()};
    const SourceHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    const CBlock block{Build()};
    Commit(block.GetHash());
    BlockValidationState state;
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/5));
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state));
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*source=*/6));
    BOOST_CHECK(hooks.sources == (std::vector<int64_t>{5, -1, 6}));
    BOOST_CHECK_EQUAL(hooks.open, 0);
}

BOOST_FIXTURE_TEST_CASE(block_extension_equivalent, SidechainSetup)
{
    // A chain whose signer can sign one block twice: a block for a header the node has, with another
    // signature its rules find as valid, is the same block. It is taken with the header's signature
    // (the node keeps that one), and is no mutated block; one the node cannot check yet is the same
    // block too (block_extension_unknown_taken). Others are mutated blocks, as without the hook.
    InstallHooks installed{std::make_unique<TwiceSignedHooks>()};
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    CBlock honest{Build()};
    honest.nVersion |= BLOCK_EXTENSION_BIT;
    honest.ext.hashSections.SetNull();
    honest.ext.sig.fill(5);
    CBlock other{honest}, unknown{honest}, forged{honest};
    other.ext.sig.fill(7);
    unknown.ext.sig.fill(8);
    forged.ext.sig.fill(6);
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{honest}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    CBlockIndex* index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index);
    {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(forged), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-extension");
    }
    // The block with the other valid signature: taken, connected, and kept with the header's.
    bool new_block{false};
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(other), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block));
    BOOST_CHECK(new_block);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) == index);
    BOOST_CHECK(*index->m_ext == honest.ext);
    {
        const FlatFilePos pos{WITH_LOCK(::cs_main, return index->GetBlockPos())};
        CBlock raw;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(raw, pos, honest.GetHash()));
        BOOST_CHECK(raw.ext == honest.ext);
    }
    // Again, either way, or with a signature the node cannot check: a duplicate, no fault. The block
    // the node has is not touched.
    for (const CBlock& again : {other, honest, unknown}) {
        BlockValidationState state;
        new_block = true;
        BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(again), state, nullptr, true, nullptr, &new_block, true)));
        BOOST_CHECK(!new_block);
        BOOST_CHECK(state.IsValid());
        BOOST_CHECK(chainman.ProcessNewBlock(std::make_shared<const CBlock>(again), /*force_processing=*/true, /*min_pow_checked=*/true, &new_block));
        BOOST_CHECK(!new_block);
    }
    CBlock read;
    BOOST_REQUIRE(chainman.m_blockman.ReadBlock(read, *index));
    BOOST_CHECK(read.ext == honest.ext);
    BOOST_CHECK(WITH_LOCK(::cs_main, return index->IsValid(BLOCK_VALID_SCRIPTS)));
}

BOOST_FIXTURE_TEST_CASE(merge_mined_outputs, SidechainSetup)
{
    // A merged mined block carries the outputs of the sidechain's own in its coinbase, after the
    // payouts; the hook is asked with the block's parent. Without the hook, none.
    const CTxOut own{0, CScript() << OP_RETURN << std::vector<unsigned char>{0xDD, 'O', 'W', 'N'}};
    const CBlock plain{Build()};
    BOOST_CHECK_EQUAL(std::count(plain.vtx[0]->vout.begin(), plain.vtx[0]->vout.end(), own), 0);
    auto owned{std::make_unique<OwnOutputHooks>()};
    OwnOutputHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    const uint256 tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};
    const CBlock block{Build()};
    BOOST_REQUIRE_EQUAL(hooks.asked.size(), 1U);
    BOOST_CHECK(hooks.asked[0] == tip);
    BOOST_CHECK_EQUAL(std::count(block.vtx[0]->vout.begin(), block.vtx[0]->vout.end(), own), 1);
    BOOST_CHECK(block.vtx[0]->vout[0] != own);
    // The block is valid with it.
    Take(block);
}

BOOST_FIXTURE_TEST_CASE(record_failures_pardoned, SidechainSetup)
{
    // A block that failed against the record of the mainchain, and the blocks on it: a header of one,
    // or on one, is no fault of the peer's, whose record may be ahead (on in the template); with the
    // hook off, punishable (Bitcoin's results). A block the operator marked invalid stays punishable.
    const CBlock first{Build()};
    Take(first);
    const CBlock second{Build()};
    Take(second);
    CBlockIndex* failed{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(first.GetHash()))};
    BlockValidationState invalidated;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(invalidated, failed, /*by_record=*/true));
    // A header on top of the second block.
    CBlockHeader next;
    next.nVersion = 0x20000000;
    next.hashPrevBlock = second.GetHash();
    next.hashMerkleRoot = uint256{5};
    next.nTime = second.nTime + 1;
    next.nBits = second.nBits;
    while (!CheckProofOfWork(next.GetHash(), next.nBits, m_node.chainman->GetConsensus())) ++next.nNonce;
    const auto result{[&](const CBlockHeader& header) {
        BlockValidationState state;
        m_node.chainman->ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state);
        return std::make_pair(state.GetResult(), state.GetRejectReason());
    }};
    for (const CBlockHeader& header : {CBlockHeader{first}, CBlockHeader{second}, next}) {
        BOOST_CHECK(result(header) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"record-failed"}));
    }
    {
        InstallHooks installed{std::make_unique<NoPardonHooks>()};
        BOOST_CHECK(result(first).first == BlockValidationResult::BLOCK_CACHED_INVALID);
        BOOST_CHECK(result(second).first == BlockValidationResult::BLOCK_CACHED_INVALID);
        BOOST_CHECK(result(next).first == BlockValidationResult::BLOCK_INVALID_PREV);
    }
    // Marked invalid by the operator: punishable all the same.
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(invalidated, failed, /*by_record=*/false));
    BOOST_CHECK(result(next).first == BlockValidationResult::BLOCK_INVALID_PREV);
    BOOST_CHECK(result(first).first == BlockValidationResult::BLOCK_CACHED_INVALID);
    // The block on it then loses its commitment as well, and is noted as failed against the record:
    // what fails it is still the operator's (the lowest failed block's), which the record does not take
    // back. Punishable all the same, and not checked again when the commitment comes back.
    CBlockIndex* above{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(second.GetHash()))};
    m_node.chainman->ActiveChainstate().InvalidateBlock(invalidated, above, /*by_record=*/true);
    BOOST_REQUIRE(Record().GetFailure(second.GetHash()) == sidechain::Mainchain::Failure::RECORD);
    BOOST_CHECK(Record().GetFailure(first.GetHash()) == sidechain::Mainchain::Failure::MANUAL);
    BOOST_CHECK(result(next).first == BlockValidationResult::BLOCK_INVALID_PREV);
    BOOST_CHECK(result(second).first == BlockValidationResult::BLOCK_CACHED_INVALID);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return m_node.chainman->ActiveChainstate().ReconsiderRecordFailure(above)));
    BOOST_CHECK(WITH_LOCK(::cs_main, return above->nStatus & BLOCK_FAILED_VALID));
    // Below it, a block that failed with no note, as one that broke a rule of the chain: the same.
    Record().ForgetFailure(first.GetHash());
    BOOST_CHECK(result(next).first == BlockValidationResult::BLOCK_INVALID_PREV);
    BOOST_CHECK(result(second).first == BlockValidationResult::BLOCK_CACHED_INVALID);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return m_node.chainman->ActiveChainstate().ReconsiderRecordFailure(above)));
}

BOOST_FIXTURE_TEST_CASE(waiting_headers_keep_their_peers, SidechainSetup)
{
    // Headers wait for their commitments; the hooks say when, and in what order, to try them again.
    // None is lost on the way: not a child tried before its parent (the hooks give both the same key),
    // nor one still waiting, which keeps the peer that sent it (its block is fetched from that peer).
    InstallHooks installed{std::make_unique<SameKeyHooks>()};
    ChainstateManager& chainman{*m_node.chainman};
    auto& waiting{chainman.m_bmm_waiting};
    WITH_LOCK(::cs_main, waiting.clear());
    // A header on `prev`; if `before_parent`, one that the store of waiting headers, ordered by hash,
    // puts before its parent.
    const auto on{[&](const CBlockHeader& prev, uint32_t salt, bool before_parent = false) {
        CBlockHeader header;
        header.nVersion = 0x20000000;
        header.hashPrevBlock = prev.GetHash();
        header.hashMerkleRoot = uint256{static_cast<uint8_t>(salt)};
        header.nTime = prev.nTime + 1;
        header.nBits = prev.nBits;
        while (!CheckProofOfWork(header.GetHash(), header.nBits, chainman.GetConsensus()) || (before_parent && !(header.GetHash() < header.hashPrevBlock))) ++header.nNonce;
        return header;
    }};
    // A chain of headers, each sent by its own peer, waiting in the reverse of the chain's order.
    std::vector<CBlockHeader> chain{CBlockHeader{Build()}};
    while (chain.size() < 8) chain.push_back(on(chain.back(), chain.size(), /*before_parent=*/true));
    // A run of headers committed to after the first, which is not: they wait too, and a header whose
    // parent nobody sent goes.
    const CBlockHeader uncommitted{on(chain.back(), 0x80)};
    const CBlockHeader after{on(uncommitted, 0x81)};
    CBlockHeader stray{on(chain.back(), 0x82)};
    stray.hashPrevBlock = uint256{0x99};
    while (!CheckProofOfWork(stray.GetHash(), stray.nBits, chainman.GetConsensus())) ++stray.nNonce;
    {
        LOCK(::cs_main);
        for (size_t i{0}; i < chain.size(); ++i) chainman.AddBmmWaiting(chain[i], /*peer=*/10 + i);
        chainman.AddBmmWaiting(uncommitted, /*peer=*/30);
        chainman.AddBmmWaiting(after, /*peer=*/30);
        chainman.AddBmmWaiting(stray, /*peer=*/31);
    }
    for (const CBlockHeader& header : chain) Commit(header.GetHash());

    const auto taken{chainman.RetryBmmWaiting()};
    BOOST_REQUIRE_EQUAL(taken.size(), chain.size());
    for (size_t i{0}; i < chain.size(); ++i) {
        // Parents first, each with its peer.
        BOOST_CHECK(taken[i].first->GetBlockHash() == chain[i].GetHash());
        BOOST_CHECK_EQUAL(taken[i].second, int64_t(10 + i));
    }
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(waiting.size(), 2U);
        // Still waiting, from the peer that sent it, and so is the header on it.
        BOOST_REQUIRE(waiting.contains(uncommitted.GetHash()));
        BOOST_CHECK_EQUAL(waiting.at(uncommitted.GetHash()).peer, 30);
        BOOST_REQUIRE(waiting.contains(after.GetHash()));
        BOOST_CHECK_EQUAL(waiting.at(after.GetHash()).peer, 30);
        BOOST_CHECK(!waiting.contains(stray.GetHash()));
    }
    // Tried again and again, the same.
    BOOST_CHECK(chainman.RetryBmmWaiting().empty());
    BOOST_CHECK(WITH_LOCK(::cs_main, return waiting.size() == 2 && waiting.at(uncommitted.GetHash()).peer == 30));
    // Once committed to, both come in, from their peer.
    Commit(uncommitted.GetHash());
    Commit(after.GetHash());
    const auto last{chainman.RetryBmmWaiting()};
    BOOST_REQUIRE_EQUAL(last.size(), 2U);
    BOOST_CHECK(last[0].first->GetBlockHash() == uncommitted.GetHash());
    BOOST_CHECK(last[1].first->GetBlockHash() == after.GetHash());
    BOOST_CHECK_EQUAL(last[0].second, 30);
    BOOST_CHECK_EQUAL(last[1].second, 30);
    BOOST_CHECK(WITH_LOCK(::cs_main, return waiting.empty()));
}

BOOST_AUTO_TEST_CASE(peg_height)
{
    // A step may say at what height the peg actions count: the bundle's lock time and the height of a
    // failure (the retry delay) then depend on it, not on the height of the block that takes them.
    auto owned{std::make_unique<StepHooks>()};
    StepHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    Consensus::SidechainParams params;
    params.bundle_retry_delay = 3;
    sidechain::Mainchain mainchain;
    AppendMain(mainchain, 100);
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay base{empty, /*journal=*/false};
    std::vector<CTxOut> payouts;
    std::string reason;
    {
        sidechain::State state{base};
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(Tx({WithdrawalOut(COIN)}, 1), 1, params, payouts, reason), reason);
    }
    hooks.step = sidechain::PegStep{.main_height = 0, .nonce = uint256{0xa}, .max_withdrawal_height = 1, .peg_height = 2};
    const auto expected{sidechain::State{base}.NextBundle(2, uint256{0xa}, params, nullptr, 1)};
    BOOST_REQUIRE(expected);
    BOOST_CHECK_EQUAL(expected->nLockTime, 2U);
    const uint256 bundle_hash{expected->GetHash().ToUint256()};
    // Blocks at different heights that take the same step build the same bundle.
    for (const int height : {5, 9}) {
        sidechain::StoreOverlay store{base, /*journal=*/true};
        sidechain::State state{store};
        CAmount minted{-1};
        BOOST_CHECK_MESSAGE(state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(bundle_hash)}}), height, params, mainchain, minted, reason), reason);
        BOOST_REQUIRE(state.Bundle());
        BOOST_CHECK(state.Bundle()->hash == bundle_hash);
        BOOST_CHECK_EQUAL(state.Bundle()->height, 2);
        BOOST_CHECK_EQUAL(Assert(state.BundleTx())->nLockTime, 2U);
    }
    // Without it, the block's height, as in the template.
    {
        hooks.step.peg_height.reset();
        sidechain::StoreOverlay store{base, /*journal=*/true};
        sidechain::State state{store};
        CAmount minted{-1};
        BOOST_CHECK(!state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(bundle_hash)}}), 5, params, mainchain, minted, reason));
        BOOST_CHECK_EQUAL(reason, "bad-sc-bundle-hash");
        hooks.step.peg_height = 2;
    }

    // The mainchain fails the bundle: the failure is at the height the step says, and the retry
    // delay counts from it, whatever the heights of the blocks.
    sidechain::StoreOverlay store{base, /*journal=*/false};
    sidechain::State state{store};
    CAmount minted{-1};
    BOOST_REQUIRE_MESSAGE(state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(bundle_hash)}}), 5, params, mainchain, minted, reason), reason);
    sidechain::MainBlock failed;
    failed.hash = uint256{0x99};
    failed.prev_hash = mainchain.TipHash();
    failed.time = 101;
    failed.bundles.push_back({bundle_hash, /*paid=*/false});
    BOOST_REQUIRE(mainchain.Append(failed));
    hooks.step = sidechain::PegStep{.main_height = 1, .nonce = uint256{0xb}, .max_withdrawal_height = 6, .peg_height = 6};
    BOOST_REQUIRE_MESSAGE(state.ConnectBlock(CoinbaseBlock(true), 50, params, mainchain, minted, reason), reason);
    BOOST_CHECK(!state.Bundle());
    BOOST_CHECK_EQUAL(state.LastFailureHeight(), 6);
    // Two after it: too early, although the block is far higher.
    hooks.step = sidechain::PegStep{.main_height = 1, .nonce = uint256{0xc}, .max_withdrawal_height = 8, .peg_height = 8};
    const auto early{sidechain::State{store}.NextBundle(9, uint256{0xc}, params, nullptr, 8)};
    BOOST_REQUIRE(early);
    BOOST_CHECK(!state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(early->GetHash().ToUint256())}}), 60, params, mainchain, minted, reason));
    BOOST_CHECK_EQUAL(reason, "bad-sc-bundle-not-allowed");
    // Three after it: the bundle again, at that height.
    hooks.step = sidechain::PegStep{.main_height = 1, .nonce = uint256{0xc}, .max_withdrawal_height = 9, .peg_height = 9};
    BOOST_REQUIRE_MESSAGE(state.ConnectBlock(CoinbaseBlock(true, {CTxOut{0, sidechain::BundleCommitScript(early->GetHash().ToUint256())}}), 61, params, mainchain, minted, reason), reason);
    BOOST_CHECK_EQUAL(state.Bundle()->height, 9);
}

BOOST_AUTO_TEST_SUITE_END()
