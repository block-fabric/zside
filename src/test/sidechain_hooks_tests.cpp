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
#include <net.h>
#include <net_processing.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <sidechain/hooks.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <sync.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
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
 * (`known`): all nines is a forged signature it takes in a header before it can tell (unchecked:
 * HeaderCheckable), which all fives replaces once it can. Connected, a block is checked again
 * (AfterBlock): a forged signature fails there, for the extension alone (ExtensionFailure).
 */
struct LateCheckedHooks : SignedHooks {
    bool known{false};
    sidechain::ChainHooks::UncheckedBudget limits;
    bool HeaderCheckable(const CBlockHeader& header, const CBlockIndex&) const override { return known || !header.IsExtended(); }
    UncheckedBudget UncheckedLimits() const override { return limits; }
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const override
    {
        if (!known) return true;
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
    bool AfterBlock(sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step, CAmount minted,
                    const Consensus::SidechainParams& params, std::string& reject_reason) const override
    {
        std::array<unsigned char, 64> valid;
        valid.fill(5);
        if (block.IsExtended() && block.ext.sig != valid) {
            reject_reason = "bad-test-sig";
            return false;
        }
        return SignedHooks::AfterBlock(state, block, height, step, minted, params, reject_reason);
    }
    bool ExtensionFailure(const std::string& reject_reason) const override { return reject_reason == "bad-test-sig"; }
};

/** Extended headers on `prev`, one on the other, with a forged signature (all nines): unchecked while the hooks cannot tell. */
std::vector<CBlockHeader> UncheckedChain(const CBlockHeader& model, const uint256& prev, size_t count, uint8_t tag)
{
    std::vector<CBlockHeader> headers;
    uint256 parent{prev};
    for (size_t i{0}; i < count; ++i) {
        CBlockHeader header{model};
        header.nVersion |= BLOCK_EXTENSION_BIT;
        header.hashPrevBlock = parent;
        header.hashMerkleRoot = uint256{tag};
        header.nNonce = static_cast<uint32_t>(i);
        header.nTime = model.nTime + static_cast<uint32_t>(i);
        header.ext.hashSections.SetNull();
        header.ext.sig.fill(9);
        headers.push_back(header);
        parent = header.GetHash();
    }
    return headers;
}

BOOST_FIXTURE_TEST_CASE(block_extension_unknown_taken, SidechainSetup)
{
    // A block for a header the node has, with another signature the node cannot check yet (it took the
    // header's, forged, before it could tell; it can once it connects the chain below): the same block
    // but for the signature, taken with the header's signature (no other one is kept). Turned away,
    // the block would stall a branch that needs it to be connected (a fork whose overtaking block it
    // is). Connected, the forged signature fails: the copy's fault, not the block's (an extension
    // failure, no fault of the peers), and the honest header, asked for again, replaces it.
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3e};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(7));
    CBlock honest{Build()};
    honest.nVersion |= BLOCK_EXTENSION_BIT;
    honest.ext.hashSections.SetNull();
    honest.ext.sig.fill(5);
    CBlock forged{honest};
    forged.ext.sig.fill(9);
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{forged}}, /*min_pow_checked=*/true, header_state, nullptr, /*source=*/7), header_state.ToString());
    CBlockIndex* index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index);
    BOOST_CHECK(WITH_LOCK(::cs_main, return index->m_unchecked && chainman.HoldsUnchecked(*index, 7)));
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
    }
    {
        // Stored with the header's (the node serves what it has; what the block hash covers is the same),
        // apart from the block files (the header is unchecked).
        CBlock raw;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(raw, *index));
        BOOST_CHECK(raw.ext == forged.ext);
        BOOST_CHECK(node::BlockManager::IsVolatile(WITH_LOCK(::cs_main, return index->GetBlockPos())));
    }
    // The node can tell now (the chain below connected): the forged signature fails as the block is
    // connected, for the extension alone: no fault of the peer, in memory only.
    hooks.known = true;
    {
        BlockValidationState state;
        chainman.ActiveChainstate().ActivateBestChain(state);
        LOCK(::cs_main);
        BOOST_CHECK(chainman.ActiveChain().Tip() != index);
        BOOST_CHECK(index->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(chainman.UncheckedExtensionFailed(*index));
        BOOST_CHECK(index->m_unchecked);
    }
    {
        // The forged header again: no fault of the peer (not "duplicate-invalid"), and nothing changes.
        BlockValidationState state;
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders({{forged}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/7));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "extension-failed");
    }
    // The honest header: its signature replaces the forged one, the failure is taken back, and the
    // block connects, a checked header from then on.
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{honest}}, /*min_pow_checked=*/true, header_state, nullptr, /*source=*/7), header_state.ToString());
    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) == index);
    BOOST_CHECK(*index->m_ext == honest.ext);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return index->m_unchecked));
    CBlock raw;
    BOOST_REQUIRE(chainman.m_blockman.ReadBlock(raw, *index));
    BOOST_CHECK(raw.ext == honest.ext);
    // Connected: in the block files, its own file deleted.
    BOOST_CHECK(!node::BlockManager::IsVolatile(WITH_LOCK(::cs_main, return index->GetBlockPos())));
    WITH_LOCK(::cs_main, chainman.CheckUnchecked());
}

/**
 * SignedHooks that keep one extended header on a parent for good (HeaderKept): a further one on the
 * same parent is spare (HeaderSpare), as a chain whose signatures cost nothing would say.
 */
struct SpareHooks : SignedHooks {
    mutable std::map<uint256, std::set<uint256>> kept;
    bool HeaderSpare(const CBlockHeader& header, const CBlockIndex& prev) const override
    {
        const auto found{kept.find(prev.GetBlockHash())};
        return header.IsExtended() && found != kept.end() && !found->second.empty();
    }
    void HeaderKept(const CBlockIndex& index) const override
    {
        if (index.m_ext && index.pprev) kept[index.pprev->GetBlockHash()].insert(index.GetBlockHash());
    }
};

BOOST_FIXTURE_TEST_CASE(spare_headers_only_on_a_branch, SidechainSetup)
{
    // A spare header (the block index has as many like it as the chain can need) from a peer: alone,
    // refused, not the peer's fault; with a header on top of it in the same message, taken unchecked
    // (the peer's, gone with it), and kept for good once its block is connected. The node's own: taken.
    auto owned{std::make_unique<SpareHooks>()};
    SpareHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3f};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    {
        LOCK(::cs_main);
        chainman.UncheckedSourceAdded(5);
        chainman.UncheckedSourceAdded(6);
    }
    const auto extended{[](CBlock block) {
        block.nVersion |= BLOCK_EXTENSION_BIT;
        block.ext.hashSections.SetNull();
        block.ext.sig.fill(5);
        return block;
    }};
    const CBlock model{Build()};
    const uint256 tip{model.hashPrevBlock};
    const CBlock a1{extended(model)};
    const CBlock a2{extended(Build(model.nTime + 1))};
    const CBlock a3{extended(Build(model.nTime + 2))};
    BOOST_REQUIRE(a1.GetHash() != a2.GetHash() && a2.GetHash() != a3.GetHash());
    CBlockHeader on_a2{a2};
    on_a2.hashPrevBlock = a2.GetHash();
    on_a2.nTime = a2.nTime + 1;
    const auto index_size{[&] { return WITH_LOCK(::cs_main, return chainman.m_blockman.m_block_index.size()); }};
    const size_t before{index_size()};

    // The first: taken, for good.
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{a1}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/5), state.ToString());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !Assert(chainman.m_blockman.LookupBlockIndex(a1.GetHash()))->m_unchecked));
    BOOST_CHECK_EQUAL(hooks.kept[tip].size(), 1U);
    // A second one alone, as a header or as a block: refused, not punished, not in the block index.
    for (int i{0}; i < 3; ++i) {
        BlockValidationState spare;
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders({{a2}}, /*min_pow_checked=*/true, spare, nullptr, /*source=*/5));
        BOOST_CHECK(spare.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK);
        BOOST_CHECK_EQUAL(spare.GetRejectReason(), "header-spare");
    }
    {
        BlockValidationState spare;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(a2), spare, nullptr, true, nullptr, nullptr, true, /*source=*/5)));
        BOOST_CHECK_EQUAL(spare.GetRejectReason(), "header-spare");
    }
    BOOST_CHECK_EQUAL(index_size(), before + 1);
    // With a header on top: taken unchecked, the peer's; gone with it.
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{a2, on_a2}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/5), state.ToString());
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(a2.GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK(index->m_unchecked && chainman.HoldsUnchecked(*index, 5));
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(5), 2U);
        chainman.UncheckedSourceGone(5);
        BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(a2.GetHash()));
        BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(on_a2.GetHash()));
        chainman.CheckUnchecked();
    }
    BOOST_CHECK_EQUAL(index_size(), before + 1);
    BOOST_CHECK_EQUAL(hooks.kept[tip].size(), 1U);
    // Again from peer 6, with its block: connected (the first has no block), it is kept for good.
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{a2, on_a2}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/6), state.ToString());
    {
        BlockValidationState block_state;
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(a2), block_state, nullptr, true, nullptr, &new_block, true, /*source=*/6)), block_state.ToString());
        BOOST_CHECK(new_block);
    }
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(a2.GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK(chainman.ActiveChain().Tip() == index);
        BOOST_CHECK(!index->m_unchecked);
        BOOST_CHECK_EQUAL(hooks.kept[tip].size(), 2U);
        chainman.UncheckedSourceGone(6);
        BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(a2.GetHash()) == index);
        BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(on_a2.GetHash()));
        chainman.CheckUnchecked();
    }
    // The node's own (its blocks, -reindex): never spare.
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({{a3}}, /*min_pow_checked=*/true, state), state.ToString());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !Assert(chainman.m_blockman.LookupBlockIndex(a3.GetHash()))->m_unchecked));
    BOOST_CHECK_EQUAL(hooks.kept[tip].size(), 3U);
}

BOOST_FIXTURE_TEST_CASE(extension_failure_not_the_peers_fault, SidechainSetup)
{
    // A block whose extension failed (taken unchecked, forged): headers on it are no fault of the peers
    // that send them ("extension-failed", BLOCK_TIME_FUTURE, not "bad-prevblk"); another extension the
    // node still cannot check replaces the failed one (it is known bad), and the block is tried again.
    InstallHooks installed{std::make_unique<LateCheckedHooks>()};
    sidechain::MainBlock first;
    first.hash = uint256{0x3c};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(3));
    CBlock forged{Build()};
    forged.nVersion |= BLOCK_EXTENSION_BIT;
    forged.ext.hashSections.SetNull();
    forged.ext.sig.fill(9);
    // Taken unchecked, and connected at once (it is on the tip): the forged signature fails there.
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(forged), /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*source=*/3));
    CBlockIndex* index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(forged.GetHash()))};
    BOOST_REQUIRE(index);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.UncheckedExtensionFailed(*index)));
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()) != index);
    BlockValidationState state;
    const std::vector<CBlockHeader> on{UncheckedChain(forged, forged.GetHash(), 1, 0x51)};
    BOOST_CHECK(!chainman.ProcessNewBlockHeaders(on, /*min_pow_checked=*/true, state, nullptr, /*source=*/3));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "extension-failed");
    // Another signature, which the node cannot check: it replaces the failed one, which is known bad.
    CBlockHeader other{forged};
    other.ext.sig.fill(6);
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{other}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/3));
    LOCK(::cs_main);
    BOOST_CHECK(*index->m_ext == other.ext);
    BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
    BOOST_CHECK(!chainman.UncheckedExtensionFailed(*index));
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

BOOST_FIXTURE_TEST_CASE(unchecked_headers_held_by_source, SidechainSetup)
{
    // Unchecked headers belong to the peers that sent them, and to the peers that sent one on top of
    // them; they leave the block index when no peer holds them. The node's own (-1) stay. Checked
    // headers are no one's.
    auto owned{std::make_unique<LateCheckedHooks>()};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    {
        LOCK(::cs_main);
        chainman.UncheckedSourceAdded(5);
        chainman.UncheckedSourceAdded(6);
    }
    const CBlock model{Build()};
    const uint256 tip{model.hashPrevBlock};
    const std::vector<CBlockHeader> chain{UncheckedChain(model, tip, 4, 0x61)};
    const std::vector<CBlockHeader> own{UncheckedChain(model, tip, 2, 0x62)};
    BlockValidationState state;
    const size_t before{WITH_LOCK(::cs_main, return chainman.m_blockman.m_block_index.size())};
    // Peer 5 sends two; peer 6 the four (it holds them all); peer 5 the rest (it holds those on its two).
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders({chain.begin(), chain.begin() + 2}, /*min_pow_checked=*/true, state, nullptr, /*source=*/5), state.ToString());
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(chain, /*min_pow_checked=*/true, state, nullptr, /*source=*/6), state.ToString());
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(own, /*min_pow_checked=*/true, state), state.ToString());
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 6U);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(5), 2U);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(6), 4U);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(-1), 2U);
        chainman.CheckUnchecked();
    }
    // A known one on top, from peer 5: it holds the chain below it too.
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{chain.back()}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/5));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainman.UncheckedHeld(5)), 4U);
    {
        // Peer 6 leaves: peer 5 holds them all, they stay.
        LOCK(::cs_main);
        chainman.UncheckedSourceGone(6);
        BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 6U);
        // Peer 5 leaves: no one holds its chain, which leaves the block index; the node's own stay.
        chainman.UncheckedSourceGone(5);
        BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 2U);
        BOOST_CHECK_EQUAL(chainman.m_blockman.m_block_index.size(), before + 2);
        for (const CBlockHeader& header : chain) BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(header.GetHash()));
        BOOST_CHECK(chainman.m_best_header && chainman.m_blockman.LookupBlockIndex(chainman.m_best_header->GetBlockHash()) == chainman.m_best_header);
        chainman.CheckUnchecked();
    }
    // A peer that is gone holds nothing: its headers are not taken (a waiting header tried again late).
    BOOST_CHECK(!chainman.ProcessNewBlockHeaders(chain, /*min_pow_checked=*/true, state, nullptr, /*source=*/5));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
}

BOOST_FIXTURE_TEST_CASE(unchecked_budget_survives_reconnections, SidechainSetup)
{
    // A peer holds at most per_peer unchecked headers; reconnecting, or coming from new addresses, does
    // not grow the block index: what a peer held goes with it (finding 6a of the review of audit 6,
    // where a peer's count went and its headers stayed).
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    hooks.limits = {.per_peer = 3};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    const CBlock model{Build()};
    const size_t before{WITH_LOCK(::cs_main, return chainman.m_blockman.m_block_index.size())};
    for (int64_t peer{10}; peer < 30; ++peer) {
        WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
        BlockValidationState state;
        const std::vector<CBlockHeader> chain{UncheckedChain(model, model.hashPrevBlock, 5, static_cast<uint8_t>(peer))};
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders(chain, /*min_pow_checked=*/true, state, nullptr, peer));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
        // Siblings too: one header each, the same budget.
        for (uint8_t k{0}; k < 5; ++k) {
            (void)chainman.ProcessNewBlockHeaders(UncheckedChain(model, model.hashPrevBlock, 1, static_cast<uint8_t>(0x80 + k)), /*min_pow_checked=*/true, state, nullptr, peer);
        }
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(peer), 3U);
        BOOST_CHECK_EQUAL(chainman.m_blockman.m_block_index.size(), before + 3);
        chainman.UncheckedSourceGone(peer);
        BOOST_CHECK_EQUAL(chainman.m_blockman.m_block_index.size(), before);
        BOOST_CHECK(chainman.m_unchecked.empty());
    }
    // Many peers at once: each has its own budget, whatever the others hold, and no more.
    for (int64_t peer{40}; peer < 50; ++peer) {
        WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
        BlockValidationState state;
        (void)chainman.ProcessNewBlockHeaders(UncheckedChain(model, model.hashPrevBlock, 5, static_cast<uint8_t>(peer)), /*min_pow_checked=*/true, state, nullptr, peer);
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(peer), 3U);
        BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 3U * (peer - 39));
    }
    LOCK(::cs_main);
    for (int64_t peer{40}; peer < 50; ++peer) chainman.UncheckedSourceGone(peer);
    BOOST_CHECK(chainman.m_unchecked.empty());
    chainman.CheckUnchecked();
}

BOOST_FIXTURE_TEST_CASE(unchecked_room_from_the_largest_sole_holder, SidechainSetup)
{
    // (Findings 6b and 7 of the review of audit 6, under the rule that replaced eviction.) A flood of an
    // attacker's headers never drops what an honest peer holds: the attacker fills its own budget and
    // no more; nobody makes room by dropping another peer's headers (unchecked_no_peer_drops_another).
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    hooks.limits = {.per_peer = 10};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    {
        LOCK(::cs_main);
        for (int64_t peer : {1, 2, 3}) chainman.UncheckedSourceAdded(peer);
    }
    const CBlock model{Build()};
    const std::vector<CBlockHeader> attack{UncheckedChain(model, model.hashPrevBlock, 6, 0x71)};
    const std::vector<CBlockHeader> honest{UncheckedChain(model, model.hashPrevBlock, 4, 0x72)};
    BlockValidationState state;
    // The attacker (peer 1) holds six; the honest peer (2) sends four, which peer 3 holds with it.
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(attack, /*min_pow_checked=*/true, state, nullptr, /*source=*/1));
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(honest, /*min_pow_checked=*/true, state, nullptr, /*source=*/2), state.ToString());
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(honest, /*min_pow_checked=*/true, state, nullptr, /*source=*/3));
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 10U);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(1), 6U);
        for (const CBlockHeader& header : attack) BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(header.GetHash()));
        for (const CBlockHeader& header : honest) BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(header.GetHash()));
    }
    // The attacker floods: it fills its own budget, then is refused.
    for (uint8_t k{0}; k < 20; ++k) {
        const bool taken{chainman.ProcessNewBlockHeaders(UncheckedChain(model, model.hashPrevBlock, 1, static_cast<uint8_t>(0xa0 + k)), /*min_pow_checked=*/true, state, nullptr, /*source=*/1)};
        BOOST_CHECK_EQUAL(taken, k < 4);
        if (!taken) BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
    }
    LOCK(::cs_main);
    for (const CBlockHeader& header : honest) BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(header.GetHash()));
    BOOST_CHECK_EQUAL(chainman.UncheckedHeld(1), 10U);
    BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 14U);
    chainman.CheckUnchecked();
}

BOOST_FIXTURE_TEST_CASE(unchecked_no_peer_drops_another, SidechainSetup)
{
    // Every peer has its whole budget of unchecked headers, whatever other peers hold or share: no
    // peer's headers are dropped to make room for another's. (The adversarial review of the design
    // that had a node-wide total: attackers holding the same headers in pairs locked the whole total,
    // so an honest peer holding nothing got none; and each new attacker connection took headers from
    // the peer that alone held the most, an honest peer first.)
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    hooks.limits = {.per_peer = 4};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    WITH_LOCK(::cs_main, for (int64_t peer{1}; peer <= 12; ++peer) chainman.UncheckedSourceAdded(peer));
    const CBlock model{Build()};
    BlockValidationState state;
    // Attackers in pairs: one sends a chain, the other its newest header (it holds the chain too).
    for (const int64_t first : {1, 3}) {
        const std::vector<CBlockHeader> chain{UncheckedChain(model, model.hashPrevBlock, 4, static_cast<uint8_t>(0x70 + first))};
        BOOST_REQUIRE(chainman.ProcessNewBlockHeaders(chain, /*min_pow_checked=*/true, state, nullptr, first));
        BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{chain.back()}}, /*min_pow_checked=*/true, state, nullptr, first + 1));
        // Over its own budget, refused: an attacker floods only itself.
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders(UncheckedChain(model, model.hashPrevBlock, 1, static_cast<uint8_t>(0x90 + first)), /*min_pow_checked=*/true, state, nullptr, first));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
    }
    // An honest peer that holds nothing gets its headers.
    const std::vector<CBlockHeader> honest{UncheckedChain(model, model.hashPrevBlock, 4, 0x7f)};
    BOOST_CHECK_MESSAGE(chainman.ProcessNewBlockHeaders(honest, /*min_pow_checked=*/true, state, nullptr, /*source=*/5), state.ToString());
    // Sybils, each with siblings of its own, take nothing from it.
    for (int64_t peer{6}; peer <= 12; ++peer) {
        BOOST_CHECK(chainman.ProcessNewBlockHeaders(UncheckedChain(model, model.hashPrevBlock, 4, static_cast<uint8_t>(0xa0 + peer)), /*min_pow_checked=*/true, state, nullptr, peer));
    }
    LOCK(::cs_main);
    for (const CBlockHeader& header : honest) BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(header.GetHash()));
    BOOST_CHECK_EQUAL(chainman.UncheckedHeld(5), 4U);
    for (int64_t peer{1}; peer <= 12; ++peer) BOOST_CHECK_EQUAL(chainman.UncheckedHeld(peer), 4U);
    BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 2U * 4 + 4 + 7 * 4);
    chainman.CheckUnchecked();
}

BOOST_FIXTURE_TEST_CASE(unchecked_inbound_budget_and_holders, SidechainSetup)
{
    // Review of audit 6, item 10: without a node-wide total, the worst case was every connection's
    // budget. Inbound peers (anyone can open those) have a much smaller budget than outbound and
    // manual ones, and at most inbound_holders of them hold unchecked headers at once: a further one
    // is refused (unchecked-budget, not punished), and nobody's headers are dropped for it. An honest
    // outbound peer still has its whole budget while any number of inbound attackers are connected.
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    hooks.limits = {.per_peer = 12, .per_inbound_peer = 3, .inbound_holders = 4};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    const CBlock model{Build()};
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(1, /*inbound=*/false));
    // Twenty inbound attackers, each with a chain of its own.
    std::map<int64_t, std::vector<CBlockHeader>> attacks;
    for (int64_t peer{100}; peer < 120; ++peer) {
        WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer, /*inbound=*/true));
        attacks[peer] = UncheckedChain(model, model.hashPrevBlock, 6, static_cast<uint8_t>(peer));
        BlockValidationState state;
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders(attacks[peer], /*min_pow_checked=*/true, state, nullptr, peer));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
        LOCK(::cs_main);
        // The first four hold the inbound budget, the others nothing.
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(peer), peer < 104 ? 3U : 0U);
        BOOST_CHECK_EQUAL(chainman.UncheckedInboundHolders(), std::min<size_t>(peer - 99, 4));
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainman.m_unchecked.size()), 4U * 3);
    // The honest outbound peer syncs a stretch of its whole budget.
    const std::vector<CBlockHeader> honest{UncheckedChain(model, model.hashPrevBlock, 12, 0x01)};
    {
        BlockValidationState state;
        BOOST_CHECK_MESSAGE(chainman.ProcessNewBlockHeaders(honest, /*min_pow_checked=*/true, state, nullptr, /*source=*/1), state.ToString());
        BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainman.UncheckedHeld(1)), 12U);
    }
    // An inbound attacker over the cap that sends the honest peer's headers holds none of them, and
    // drops nothing (they are the honest peer's).
    {
        BlockValidationState state;
        (void)chainman.ProcessNewBlockHeaders(honest, /*min_pow_checked=*/true, state, nullptr, /*source=*/110);
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(110), 0U);
        BOOST_CHECK_EQUAL(chainman.UncheckedHeld(1), 12U);
        for (const CBlockHeader& header : honest) BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(header.GetHash()));
        BOOST_CHECK_EQUAL(chainman.m_unchecked.size(), 4U * 3 + 12);
        chainman.CheckUnchecked();
    }
    // An inbound holder leaves: another inbound peer may hold headers in its place.
    {
        LOCK(::cs_main);
        chainman.UncheckedSourceGone(100);
        BOOST_CHECK_EQUAL(chainman.UncheckedInboundHolders(), 3U);
    }
    {
        BlockValidationState state;
        BOOST_CHECK(chainman.ProcessNewBlockHeaders(std::vector<CBlockHeader>{attacks[115].begin(), attacks[115].begin() + 3}, /*min_pow_checked=*/true, state, nullptr, 115));
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders(attacks[116], /*min_pow_checked=*/true, state, nullptr, 116));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
    }
    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(chainman.UncheckedHeld(115), 3U);
    BOOST_CHECK_EQUAL(chainman.UncheckedInboundHolders(), 4U);
    BOOST_CHECK_EQUAL(chainman.UncheckedHeld(1), 12U);
    chainman.CheckUnchecked();
    for (int64_t peer{101}; peer < 120; ++peer) chainman.UncheckedSourceGone(peer);
    chainman.UncheckedSourceGone(1);
    BOOST_CHECK(chainman.m_unchecked.empty());
    BOOST_CHECK(chainman.m_unchecked_inbound.empty());
}

BOOST_FIXTURE_TEST_CASE(unchecked_blocks_stored_apart, SidechainSetup)
{
    // The block of an unchecked header is stored apart from the block files, in a file of its own,
    // within the budget of its sender, charged to the header's holders; deleted with the header. So
    // a peer that sends blocks of headers it then takes away (disconnecting), again and again, leaves
    // nothing on disk (the adversarial review: blocks written to the block files for ever).
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    const auto forged{[&](int64_t time) {
        CBlock block{Build(time)};
        block.nVersion |= BLOCK_EXTENSION_BIT;
        block.ext.hashSections.SetNull();
        block.ext.sig.fill(9);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
        return block;
    }};
    const auto send{[&](const CBlock& block, int64_t peer) {
        BlockValidationState state;
        bool new_block{false};
        LOCK(::cs_main);
        BOOST_CHECK_MESSAGE(chainman.AcceptBlock(std::make_shared<const CBlock>(block), state, nullptr, /*fRequested=*/true, nullptr, &new_block, true, peer), state.ToString());
        return new_block;
    }};
    const CBlock block{forged(GetTime() + 1)};
    const size_t size{::GetSerializeSize(TX_WITH_WITNESS(block))};
    std::optional<FlatFilePos> first;
    for (int64_t peer{10}; peer < 15; ++peer) {
        WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
        BOOST_CHECK(send(block, peer));
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(block.GetHash())};
        BOOST_REQUIRE(index && index->m_unchecked && (index->nStatus & BLOCK_HAVE_DATA));
        const FlatFilePos pos{index->GetBlockPos()};
        BOOST_CHECK(node::BlockManager::IsVolatile(pos));
        BOOST_CHECK_EQUAL(chainman.UncheckedBytes(peer), size);
        // The same file each time: what went before was deleted.
        if (!first) first = pos;
        BOOST_CHECK(pos == *first);
        const fs::path file{chainman.m_blockman.GetBlockPosFilename(pos)};
        BOOST_CHECK(fs::exists(file));
        chainman.CheckUnchecked();
        chainman.UncheckedSourceGone(peer);
        BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(block.GetHash()));
        BOOST_CHECK(!fs::exists(file));
        BOOST_CHECK_EQUAL(chainman.UncheckedBytes(peer), 0U);
    }

    // The budget of the sender: a block over it is not stored (no fault of the peer's).
    hooks.limits = {.per_peer = 10, .block_bytes_per_peer = size + size / 2};
    WITH_LOCK(::cs_main, for (int64_t peer : {20, 21, 22}) chainman.UncheckedSourceAdded(peer));
    const CBlock a{forged(GetTime() + 2)}, b{forged(GetTime() + 3)}, c{forged(GetTime() + 4)};
    BOOST_CHECK(send(a, 20));
    BOOST_CHECK(!send(b, 20));
    {
        LOCK(::cs_main);
        BOOST_CHECK(!(chainman.m_blockman.LookupBlockIndex(b.GetHash())->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK_EQUAL(chainman.UncheckedBytes(20), size);
    }
    // An inbound peer's budget of bytes is the inbound one, smaller: its block is not stored.
    hooks.limits = {.per_peer = 10, .block_bytes_per_peer = size + size / 2, .block_bytes_per_inbound_peer = size / 2};
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(23, /*inbound=*/true));
    const CBlock d{forged(GetTime() + 5)};
    BOOST_CHECK(!send(d, 23));
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(d.GetHash())};
        BOOST_REQUIRE(index && chainman.HoldsUnchecked(*index, 23));
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK_EQUAL(chainman.UncheckedBytes(23), 0U);
        chainman.UncheckedSourceGone(23);
    }
    // Charged to every holder: one it does not fit stops holding the header and those on it. Peer 21
    // holds c and a header on it, and a's block (it holds a too): full. Peer 22 sends c's block.
    BlockValidationState state;
    CBlockHeader after_c{c};
    ++after_c.nTime;
    const std::vector<CBlockHeader> on_c{UncheckedChain(after_c, c.GetHash(), 1, 0x5c)};
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{c}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/21));
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(on_c, /*min_pow_checked=*/true, state, nullptr, /*source=*/21), state.ToString());
    BOOST_REQUIRE(chainman.ProcessNewBlockHeaders({{a}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/21));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainman.UncheckedBytes(21)), size);
    BOOST_CHECK(send(c, 22));
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(c.GetHash())};
        BOOST_REQUIRE(index && (index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK(!chainman.HoldsUnchecked(*index, 21));
        BOOST_CHECK(chainman.HoldsUnchecked(*index, 22));
        // No one holds the header on c any more: gone.
        BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(on_c[0].GetHash()));
        BOOST_CHECK_EQUAL(chainman.UncheckedBytes(21), size);
        BOOST_CHECK_EQUAL(chainman.UncheckedBytes(22), size);
        chainman.CheckUnchecked();
    }
    // Nor does a peer hold headers whose blocks are over its budget (a known one: taken, not held).
    BOOST_CHECK(chainman.ProcessNewBlockHeaders({{c}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/20));
    LOCK(::cs_main);
    BOOST_CHECK(!chainman.HoldsUnchecked(*chainman.m_blockman.LookupBlockIndex(c.GetHash()), 20));
    BOOST_CHECK_EQUAL(chainman.UncheckedBytes(20), size);
    for (int64_t peer : {20, 21, 22}) chainman.UncheckedSourceGone(peer);
    BOOST_CHECK(chainman.m_unchecked.empty());
    chainman.CheckUnchecked();
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
        BOOST_CHECK(taken[i].first == chain[i].GetHash());
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
    BOOST_CHECK(last[0].first == uncommitted.GetHash());
    BOOST_CHECK(last[1].first == after.GetHash());
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

namespace {
/** Runs `run` once, when the active tip changes: synchronously, with cs_main held, as ActivateBestChain is between two blocks. */
struct OnTipChange final : CValidationInterface {
    std::function<void()> run;
    void ActiveTipChange(const CBlockIndex&, bool) override
    {
        if (!run) return;
        const auto once{std::move(run)};
        run = nullptr;
        once();
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(attack_holder_leaves_while_its_blocks_connect, SidechainSetup)
{
    // ATTACK THAT SUCCEEDS (this test fails on the code it was written against): ActivateBestChain
    // keeps pindexMostWork across the iterations of its outer loop, and releases cs_main between them
    // (after each block that betters the tip). If the last holder of the unchecked headers it connects
    // to disconnects meanwhile (FinalizeNode, in the socket handler thread, takes cs_main there),
    // EraseUnchecked takes the rest of the branch out of the block index and deletes their blocks,
    // and the next iteration connects on toward the erased entry: reading its block fails, a fatal
    // error, and the node shuts down. (Were the read to succeed, a dead entry would join the active
    // chain, freed MAX_ERASED_HEADERS erasures later.) Any peer whose valid blocks of unchecked
    // headers the node connects -- valid blocks of the honest chain it relays, out of order so that
    // ActivateBestChain connects several, before an honest peer sent the same headers -- crashes the
    // node by disconnecting at the right time; so does an honest peer that drops while the node
    // connects what it alone sent. Here, the peer leaves (as FinalizeNode would) in the
    // ActiveTipChange callback, once the first block is connected.
    auto owned{std::make_unique<LateCheckedHooks>()};
    LateCheckedHooks& hooks{*owned};
    InstallHooks installed{std::move(owned)};
    sidechain::MainBlock first;
    first.hash = uint256{0x3e};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    constexpr int64_t peer{7};
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
    // Three valid blocks (signed: all fives) on the tip, one on the other.
    const int height{WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()) + 1};
    std::vector<CBlock> blocks;
    CBlock block{Build()};
    block.nVersion |= BLOCK_EXTENSION_BIT;
    block.ext.hashSections.SetNull();
    block.ext.sig.fill(5);
    blocks.push_back(block);
    for (int i{1}; i < 3; ++i) {
        CBlock next{blocks.back()};
        next.hashPrevBlock = blocks.back().GetHash();
        next.nTime = blocks.back().nTime + 1;
        CMutableTransaction coinbase{*next.vtx[0]};
        coinbase.vin[0].scriptSig = CScript() << (height + i) << OP_0;
        next.vtx[0] = MakeTransactionRef(std::move(coinbase));
        next.hashMerkleRoot = BlockMerkleRoot(next);
        blocks.push_back(next);
    }
    // The peer sends their headers (unchecked: it holds them) and their blocks (stored apart).
    std::vector<CBlockHeader> headers{blocks.begin(), blocks.end()};
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state, nullptr, peer), state.ToString());
    for (const CBlock& b : blocks) {
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(WITH_LOCK(::cs_main, return chainman.AcceptBlock(std::make_shared<const CBlock>(b), state, nullptr, /*fRequested=*/true, nullptr, &new_block, true, peer)), state.ToString());
        BOOST_REQUIRE(new_block);
    }
    {
        LOCK(::cs_main);
        for (const CBlock& b : blocks) {
            const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(b.GetHash())};
            BOOST_REQUIRE(index && index->m_unchecked && (index->nStatus & BLOCK_HAVE_DATA));
        }
    }
    // The node can check them now (the chain below is connected): they are valid.
    hooks.known = true;
    auto leaves{std::make_shared<OnTipChange>()};
    leaves->run = [&] {
        LOCK(::cs_main);
        chainman.UncheckedSourceGone(peer);
    };
    m_node.validation_signals->RegisterSharedValidationInterface(leaves);
    BlockValidationState abc_state;
    const bool activated{chainman.ActiveChainstate().ActivateBestChain(abc_state)};
    m_node.validation_signals->UnregisterSharedValidationInterface(leaves);
    BOOST_TEST_MESSAGE("ActivateBestChain: " << (activated ? "ok" : "failed") << " (" << abc_state.ToString() << ")");
    // The peer leaving is no error of the node's: the first block is connected, and the node goes on.
    BOOST_CHECK_MESSAGE(activated, abc_state.ToString());
    BOOST_CHECK(!abc_state.IsError());
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    BOOST_CHECK(tip->GetBlockHash() == blocks[0].GetHash());
    BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(tip->GetBlockHash()) == tip);
    for (const CBlockIndex* index{tip}; index; index = index->pprev) {
        BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(index->GetBlockHash()) == index);
    }
}

namespace {
/** LateCheckedHooks whose extended headers wait (as for a commitment not on record) while `wait` is set. */
struct WaitingLateHooks final : LateCheckedHooks {
    bool wait{true};
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const override
    {
        if (wait) return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "test-wait", "waits");
        return LateCheckedHooks::CheckHeader(header, prev, chainman, state);
    }
    bool IsWaitReason(const std::string& reject_reason) const override { return reject_reason == "test-wait" || LateCheckedHooks::IsWaitReason(reject_reason); }
    std::optional<int64_t> WaitingReady(const CBlockHeader&, const sidechain::Mainchain&) const override
    {
        if (wait) return std::nullopt;
        return 0;
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(waiting_unchecked_header_is_never_the_nodes_own, SidechainSetup)
{
    // A header that waits is kept without a peer by validation (a block message: the net layer tells
    // who sent it). Tried again, one whose sender is not known is not taken as the node's own
    // (UNKNOWN_SOURCE): it holds no unchecked header. One whose peer is known is taken within its budget.
    auto owned{std::make_unique<WaitingLateHooks>()};
    WaitingLateHooks& hooks{*owned};
    hooks.limits.per_peer = 10;
    InstallHooks installed{std::move(owned)};
    ChainstateManager& chainman{*m_node.chainman};
    WITH_LOCK(::cs_main, chainman.m_bmm_waiting.clear());
    constexpr int64_t peer{7};
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
    const auto unchecked_block{[&](uint8_t salt) {
        CBlock block{Build()};
        block.nVersion |= BLOCK_EXTENSION_BIT;
        block.ext.hashSections.SetNull();
        block.ext.sig.fill(5);
        CMutableTransaction coinbase{*block.vtx[0]};
        coinbase.vin[0].scriptSig << static_cast<int64_t>(salt);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        return block;
    }};
    // A block from the peer, as a `block` message: it waits, without a peer (validation does not know it).
    const CBlock unknown{unchecked_block(1)};
    bool new_block{false};
    chainman.ProcessNewBlock(std::make_shared<const CBlock>(unknown), /*force_processing=*/false, /*min_pow_checked=*/true, &new_block, peer);
    BOOST_REQUIRE(!new_block);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.m_bmm_waiting.at(unknown.GetHash()).peer), -1);
    // Another, whose sender the net layer recorded.
    const CBlock known{unchecked_block(2)};
    {
        LOCK(::cs_main);
        chainman.AddBmmWaiting(known, peer);
    }
    hooks.wait = false;
    const auto taken{chainman.RetryBmmWaiting()};
    BOOST_REQUIRE_EQUAL(taken.size(), 1U);
    BOOST_CHECK(taken[0].first == known.GetHash());
    BOOST_CHECK_EQUAL(taken[0].second, peer);
    LOCK(::cs_main);
    // The one without a peer is not in the block index, nor waiting; the node holds nothing.
    BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(unknown.GetHash()));
    BOOST_CHECK(!chainman.m_bmm_waiting.contains(unknown.GetHash()));
    BOOST_CHECK_EQUAL(chainman.UncheckedHeld(-1), 0U);
    BOOST_CHECK_EQUAL(chainman.UncheckedHeld(ChainstateManager::UNKNOWN_SOURCE), 0U);
    // The other is the peer's, and leaves with it.
    const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(known.GetHash())};
    BOOST_REQUIRE(index && index->m_unchecked);
    BOOST_CHECK(chainman.HoldsUnchecked(*index, peer));
    chainman.CheckUnchecked();
    chainman.UncheckedSourceGone(peer);
    BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(known.GetHash()));
    BOOST_CHECK(chainman.m_unchecked.empty());
    chainman.CheckUnchecked();
}

namespace {
/**
 * Takes the unchecked headers `peer` holds out of the block index where code uses the entries it looked
 * up (TestBlockIndexUse): from another thread, if cs_main is free there, as any thread may whenever the
 * code does not hold it (FinalizeNode, in the socket handler). Once.
 */
struct EraseWhereUsed {
    ChainstateManager& chainman;
    const int64_t peer;
    bool erased{false};
    EraseWhereUsed(ChainstateManager& chainman_in, int64_t peer_in) : chainman{chainman_in}, peer{peer_in}
    {
        g_test_block_index_use = [this] {
            if (erased) return;
            std::thread other{[this] {
                TRY_LOCK(::cs_main, locked);
                if (!locked) return;
                chainman.UncheckedSourceGone(peer);
                erased = true;
            }};
            other.join();
        };
    }
    ~EraseWhereUsed() { g_test_block_index_use = nullptr; }
};

UniValue CallRpc(node::NodeContext& node, const std::string& method, std::vector<UniValue> params)
{
    JSONRPCRequest request;
    request.context = &node;
    request.strMethod = method;
    request.params = UniValue{UniValue::VARR};
    for (UniValue& param : params) request.params.push_back(std::move(param));
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    return tableRPC.execute(request);
}
} // namespace

BOOST_FIXTURE_TEST_CASE(unchecked_erased_while_used, SidechainSetup)
{
    // Unchecked headers leave the block index, and are freed, once no peer holds them: at any time
    // cs_main is free (a peer disconnecting). No code may use one it looked up once it released the
    // lock (the entries kept a while before they are freed, m_erased_headers, are only defence in
    // depth). Here a peer leaves exactly where each RPC uses what it looked up, if cs_main is free
    // there, and the entries that leave are overwritten (m_test_poison_erased): the RPCs that show
    // unchecked headers hold the lock while they do (they show what is there), and InvalidateBlock,
    // which releases it between its steps, finds them again.
    InstallHooks installed{std::make_unique<LateCheckedHooks>()};
    sidechain::MainBlock first;
    first.hash = uint256{0x3e};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    constexpr int64_t peer{7};
    WITH_LOCK(::cs_main, chainman.m_test_poison_erased = true);
    const int height{WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()) + 1};
    // A block of an unchecked header (forged: it fails when connected, and stays, with its block,
    // unchecked) and two headers on it.
    CBlock forged{Build()};
    forged.nVersion |= BLOCK_EXTENSION_BIT;
    forged.ext.hashSections.SetNull();
    forged.ext.sig.fill(9);
    while (!CheckProofOfWork(forged.GetHash(), forged.nBits, chainman.GetConsensus())) ++forged.nNonce;
    CBlockHeader later{forged};
    later.nTime = forged.nTime + 1;
    const std::vector<CBlockHeader> on{UncheckedChain(later, forged.GetHash(), 2, 0x92)};
    const auto send{[&] {
        WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
        std::vector<CBlockHeader> headers{forged};
        headers.insert(headers.end(), on.begin(), on.end());
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state, nullptr, peer), state.ToString());
        BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(forged), /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, peer));
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(forged.GetHash())};
        BOOST_REQUIRE(index && index->m_unchecked && (index->nStatus & BLOCK_HAVE_DATA));
        BOOST_REQUIRE(chainman.m_blockman.LookupBlockIndex(on.back().GetHash()));
    }};
    const auto gone{[&] {
        LOCK(::cs_main);
        return !chainman.m_blockman.LookupBlockIndex(forged.GetHash()) && !chainman.m_blockman.LookupBlockIndex(on.back().GetHash());
    }};

    // getblockheader: the lock held throughout, nothing leaves while it shows the header.
    send();
    {
        EraseWhereUsed leaves{chainman, peer};
        const UniValue header{CallRpc(m_node, "getblockheader", {UniValue{on.back().GetHash().GetHex()}})};
        BOOST_CHECK(!leaves.erased);
        BOOST_CHECK_EQUAL(header["height"].getInt<int>(), height + 2);
        BOOST_CHECK_EQUAL(header["previousblockhash"].get_str(), on.front().GetHash().GetHex());
        BOOST_CHECK_EQUAL(header["time"].getInt<int64_t>(), on.back().nTime);
    }
    WITH_LOCK(::cs_main, chainman.UncheckedSourceGone(peer));
    BOOST_REQUIRE(gone());

    // getchaintips: likewise.
    send();
    {
        EraseWhereUsed leaves{chainman, peer};
        const UniValue tips{CallRpc(m_node, "getchaintips", {})};
        BOOST_CHECK(!leaves.erased);
        bool found{false};
        for (const UniValue& tip : tips.getValues()) {
            if (tip["hash"].get_str() != on.back().GetHash().GetHex()) continue;
            found = true;
            BOOST_CHECK_EQUAL(tip["height"].getInt<int>(), height + 2);
            BOOST_CHECK_EQUAL(tip["branchlen"].getInt<int>(), 3);
        }
        BOOST_CHECK(found);
    }
    WITH_LOCK(::cs_main, chainman.UncheckedSourceGone(peer));
    BOOST_REQUIRE(gone());

    // getblock of the unchecked header's block: the lock held throughout (the block, stored apart, is
    // deleted with the header).
    send();
    {
        EraseWhereUsed leaves{chainman, peer};
        const UniValue block{CallRpc(m_node, "getblock", {UniValue{forged.GetHash().GetHex()}, UniValue{1}})};
        BOOST_CHECK(!leaves.erased);
        BOOST_CHECK_EQUAL(block["height"].getInt<int>(), height);
        BOOST_CHECK_EQUAL(block["hash"].get_str(), forged.GetHash().GetHex());
        BOOST_CHECK_EQUAL(block["nTx"].getInt<int>(), forged.vtx.size());
    }
    // getblock of a checked block: read without the lock, as ever; the peer leaves meanwhile, which
    // changes nothing for it (checked entries never leave).
    {
        EraseWhereUsed leaves{chainman, peer};
        const uint256 tip{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash())};
        const UniValue block{CallRpc(m_node, "getblock", {UniValue{tip.GetHex()}, UniValue{1}})};
        BOOST_CHECK(leaves.erased);
        BOOST_CHECK_EQUAL(block["height"].getInt<int>(), height - 1);
    }
    BOOST_REQUIRE(gone());
    // The wallet's interface: the same (the block of an unchecked header read with the lock held).
    send();
    {
        EraseWhereUsed leaves{chainman, peer};
        CBlock read;
        int found_height{-2};
        BOOST_CHECK(m_node.chain->findBlock(forged.GetHash(), interfaces::FoundBlock().height(found_height).data(read)));
        BOOST_CHECK(!leaves.erased);
        BOOST_CHECK_EQUAL(found_height, height);
        BOOST_CHECK(read.GetHash() == forged.GetHash());
    }
    WITH_LOCK(::cs_main, chainman.UncheckedSourceGone(peer));
    BOOST_REQUIRE(gone());

    // The RPCs that use what they look up without the lock do not see unchecked headers at all.
    send();
    {
        EraseWhereUsed leaves{chainman, peer};
        BOOST_CHECK_THROW(CallRpc(m_node, "getblockstats", {UniValue{forged.GetHash().GetHex()}}), UniValue);
        BOOST_CHECK_THROW(CallRpc(m_node, "gettxoutproof", {UniValue{UniValue::VARR}, UniValue{forged.GetHash().GetHex()}}), UniValue);
        BOOST_CHECK(!leaves.erased);
    }
    WITH_LOCK(::cs_main, chainman.UncheckedSourceGone(peer));
    BOOST_REQUIRE(gone());

    // InvalidateBlock releases cs_main between its steps, with a map of the blocks that may become
    // the best header or a candidate: an unchecked branch with more work than the tip it takes back
    // is among them. That branch leaves meanwhile (the peer goes as InvalidateBlock begins): it must
    // not become the best header, nor a candidate (here, without overwriting the entries that left,
    // which would hide it: it would point to an entry no longer in the block index).
    WITH_LOCK(::cs_main, chainman.m_test_poison_erased = false);
    const CBlock valid{Build()};
    Take(valid);
    // The branch: on the tip's parent, two unchecked headers (more work than the tip).
    WITH_LOCK(::cs_main, chainman.UncheckedSourceAdded(peer));
    const std::vector<CBlockHeader> branch{UncheckedChain(valid, valid.hashPrevBlock, 3, 0x93)};
    BlockValidationState state;
    BOOST_REQUIRE_MESSAGE(chainman.ProcessNewBlockHeaders(branch, /*min_pow_checked=*/true, state, nullptr, peer), state.ToString());
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.m_best_header->GetBlockHash()) == branch.back().GetHash());
    {
        EraseWhereUsed leaves{chainman, peer};
        BOOST_CHECK(chainman.ActiveChainstate().InvalidateBlock(state, valid.GetHash()));
        BOOST_CHECK(leaves.erased);
    }
    LOCK(::cs_main);
    BOOST_CHECK(chainman.ActiveChain().Tip()->GetBlockHash() == valid.hashPrevBlock);
    for (const CBlockHeader& header : branch) BOOST_CHECK(!chainman.m_blockman.LookupBlockIndex(header.GetHash()));
    const CBlockIndex* best{chainman.m_best_header};
    BOOST_CHECK(best && chainman.m_blockman.LookupBlockIndex(best->GetBlockHash()) == best);
    for (const CBlockIndex* candidate : chainman.ActiveChainstate().setBlockIndexCandidates) {
        BOOST_CHECK(chainman.m_blockman.LookupBlockIndex(candidate->GetBlockHash()) == candidate);
    }
    chainman.CheckBlockIndex();
    chainman.CheckUnchecked();
}

namespace {
/** A peer of the node's (PeerManager), whose messages the test sends. */
struct TestPeer {
    CNode node;
    TestPeer(NodeId id, ConnmanTestMsg& connman) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
        : node{id, /*sock=*/nullptr, CAddress{CService{CNetAddr{in_addr{htonl(0x0a000001 + static_cast<uint32_t>(id))}}, 8333}, NODE_NETWORK},
               /*nKeyedNetGroupIn=*/0, /*nLocalHostNonceIn=*/0, CAddress{}, /*addrNameIn=*/"", ConnectionType::OUTBOUND_FULL_RELAY,
               /*inbound_onion=*/false, /*network_key=*/0}
    {
        connman.Handshake(node, /*successfully_connected=*/true, ServiceFlags(NODE_NETWORK | NODE_WITNESS), ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                          PROTOCOL_VERSION, /*relay_txs=*/false);
    }
    void Send(ConnmanTestMsg& connman, CSerializedNetMsg&& msg) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        // (What the node sent the peer is dropped: the transport takes one message at a time.)
        connman.FlushSendBuffer(node);
        (void)connman.ReceiveMsgFrom(node, std::move(msg));
        node.fPauseSend = false;
        while (connman.ProcessMessagesOnce(node)) {}
        connman.FlushSendBuffer(node);
    }
    void Headers(ConnmanTestMsg& connman, const std::vector<CBlockHeader>& headers) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        std::vector<CBlock> blocks;
        for (const CBlockHeader& header : headers) blocks.emplace_back(header);
        Send(connman, NetMsg::Make(NetMsgType::HEADERS, TX_WITH_WITNESS(blocks)));
    }
    void Block(ConnmanTestMsg& connman, const CBlock& block) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        Send(connman, NetMsg::Make(NetMsgType::BLOCK, TX_WITH_WITNESS(block)));
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(extension_failure_asked_again_whatever_fails, SidechainSetup)
{
    // Review of audit 6, item 8a: the blocks that failed with a forged extension were asked for again
    // from a list of at most 64 (m_extension_reask); an attacker filled it with blocks of its own that
    // failed so, and an honest block's failure was then never asked for again: its branch stalled. That
    // list is gone (a failure asks the peers that hold the header, or whose best chain has it, for its
    // headers again, one pending request a peer, from where the pending one and the new one meet), and
    // nothing is kept for a failure that an attacker could fill. Here an attacker makes 150 blocks of
    // its own fail before, and 150 after, the honest block's failure (the forged copy of its header
    // the attacker sent first): the honest peer is still asked, and its answer connects the block.
    LOCK(NetEventsInterface::g_msgproc_mutex);
    InstallHooks installed{std::make_unique<LateCheckedHooks>()};
    sidechain::MainBlock first;
    first.hash = uint256{0x3e};
    BOOST_REQUIRE(Record().Append(first));
    ChainstateManager& chainman{*m_node.chainman};
    ConnmanTestMsg& connman{static_cast<ConnmanTestMsg&>(*m_node.connman)};
    connman.SetPeerConnectTimeout(99999s);
    // (As the node does: the test setup does not.)
    m_node.validation_signals->RegisterValidationInterface(m_node.peerman.get());
    TestPeer honest{0, connman};
    TestPeer attacker{1, connman};
    const uint256 tip{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash())};

    CBlock block{Build()};
    block.nVersion |= BLOCK_EXTENSION_BIT;
    block.ext.hashSections.SetNull();
    block.ext.sig.fill(5);
    CBlockHeader forged{block};
    forged.ext.sig.fill(9);
    // Blocks of the attacker's own on the tip, each failing for its forged extension when connected.
    int tag{0};
    const auto fail_own{[&](int count) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex) {
        for (int i{0}; i < count; ++i) {
            CBlock own{Build(block.nTime + 1 + tag)};
            CMutableTransaction coinbase{*own.vtx[0]};
            ++tag;
            coinbase.vin[0].scriptSig << std::vector<unsigned char>{static_cast<unsigned char>(tag), static_cast<unsigned char>(tag >> 8), 0xa7};
            own.vtx[0] = MakeTransactionRef(std::move(coinbase));
            own.nVersion |= BLOCK_EXTENSION_BIT;
            own.ext.hashSections.SetNull();
            own.ext.sig.fill(9);
            own.hashMerkleRoot = BlockMerkleRoot(own);
            while (!CheckProofOfWork(own.GetHash(), own.nBits, chainman.GetConsensus())) ++own.nNonce;
            attacker.Block(connman, own);
            LOCK(::cs_main);
            const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(own.GetHash())};
            BOOST_REQUIRE(index && chainman.UncheckedExtensionFailed(*index));
        }
    }};
    fail_own(150);
    // The attacker sends the honest header with a forged signature first; the honest peer its own
    // (another signature the node cannot check: it keeps the first), then the block, which fails.
    attacker.Headers(connman, {forged});
    honest.Headers(connman, {block});
    honest.Block(connman, block);
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(block.GetHash())};
        BOOST_REQUIRE(index && chainman.UncheckedExtensionFailed(*index));
        BOOST_REQUIRE(chainman.HoldsUnchecked(*index, honest.node.GetId()));
        BOOST_CHECK(chainman.ActiveChain().Tip()->GetBlockHash() == tip);
    }
    fail_own(150);
    // The honest peer is asked for the headers after the tip (the block's parent).
    std::optional<CBlockLocator> asked;
    const auto capture_orig{CaptureMessage};
    m_node.connman->SetCaptureMessages(true);
    CaptureMessage = [&](const CAddress&, const std::string& msg_type, std::span<const unsigned char> data, bool is_incoming) {
        if (is_incoming || msg_type != NetMsgType::GETHEADERS) return;
        CBlockLocator locator;
        SpanReader{data} >> locator;
        asked = locator;
    };
    // (Past the wait between two getheaders to one peer.)
    SetMockTime(std::chrono::seconds{GetTime() + 5 * 60});
    m_node.peerman->SendMessages(honest.node);
    CaptureMessage = capture_orig;
    m_node.connman->SetCaptureMessages(false);
    BOOST_REQUIRE(asked && !asked->vHave.empty());
    BOOST_CHECK(asked->vHave.front() == tip);
    // Its answer: its own signature replaces the forged one, and the block is connected.
    honest.Headers(connman, {block});
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()) == block.GetHash());
    BOOST_CHECK(!honest.node.fDisconnect);
    m_node.peerman->FinalizeNode(honest.node);
    m_node.peerman->FinalizeNode(attacker.node);
    m_node.validation_signals->UnregisterValidationInterface(m_node.peerman.get());
}

BOOST_AUTO_TEST_SUITE_END()
