// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// A sidechain's unchecked headers (sidechain::ChainHooks::HeaderCheckable) under fuzzing: peers come
// and go, and send headers nobody can check, on the tip or on each other's, in any order. Whatever
// they do, the node holds no more than its budget (each peer's, and the total), every unchecked
// header has a holder that holds those below it, the block index holds exactly the checked headers
// and the unchecked ones (what no one holds is gone, with nothing pointing to it), and a peer that
// leaves takes what it alone held with it.

#include <chain.h>
#include <primitives/block.h>
#include <sidechain/hooks.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>

#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace {
const TestingSetup* g_setup;

/** Extended headers are of the chain's own kind, checked by nobody now (all taken unchecked). */
struct UncheckedHooks : sidechain::ChainHooks {
    UncheckedBudget limits;
    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override { return header.IsExtended() ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM; }
    bool CheckHeader(const CBlockHeader&, const CBlockIndex&, const ChainstateManager&, BlockValidationState&) const override { return true; }
    bool HeaderCheckable(const CBlockHeader& header, const CBlockIndex&) const override { return !header.IsExtended(); }
    UncheckedBudget UncheckedLimits() const override { return limits; }
};

void initialize_unchecked()
{
    static const auto testing_setup{MakeNoLogFileContext<const TestingSetup>(ChainType::REGTEST, {.extra_args = {"-sidechainslot=3"}})};
    g_setup = testing_setup.get();
}
} // namespace

FUZZ_TARGET(sidechain_unchecked, .init = initialize_unchecked)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    SetMockTime(ConsumeTime(fdp, /*min=*/1700000000, /*max=*/1800000000));
    ChainstateManager& chainman{*g_setup->m_node.chainman};
    auto owned{std::make_unique<UncheckedHooks>()};
    owned->limits = {.per_peer = fdp.ConsumeIntegralInRange<size_t>(1, 12), .total = fdp.ConsumeIntegralInRange<size_t>(1, 30)};
    const sidechain::ChainHooks::UncheckedBudget limits{owned->limits};
    sidechain::SetHooks(std::move(owned));

    const CBlockIndex* tip{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip())};
    const size_t base{WITH_LOCK(::cs_main, return chainman.m_blockman.m_block_index.size())};
    std::set<int64_t> live;
    std::vector<uint256> sent;
    uint32_t serial{0};

    LIMITED_WHILE(fdp.remaining_bytes() > 0, 400)
    {
        const int64_t peer{fdp.ConsumeIntegralInRange<int64_t>(0, 7)};
        CallOneOf(
            fdp,
            [&] {
                LOCK(::cs_main);
                chainman.UncheckedSourceAdded(peer);
                live.insert(peer);
            },
            [&] {
                std::vector<const CBlockIndex*> alone;
                {
                    LOCK(::cs_main);
                    for (const auto& [index, entry] : chainman.m_unchecked) {
                        if (entry.holders.size() == 1 && entry.holders.front() == peer) alone.push_back(index);
                    }
                }
                std::vector<uint256> hashes;
                for (const CBlockIndex* index : alone) hashes.push_back(index->GetBlockHash());
                LOCK(::cs_main);
                chainman.UncheckedSourceGone(peer);
                live.erase(peer);
                // What it alone held is gone.
                for (const uint256& hash : hashes) assert(!chainman.m_blockman.LookupBlockIndex(hash));
            },
            [&] {
                // Headers on the tip or on one sent before (if still there), one on the other.
                uint256 parent{tip->GetBlockHash()};
                uint32_t time{tip->nTime + 1};
                if (!sent.empty() && fdp.ConsumeBool()) {
                    const uint256 pick{sent[fdp.ConsumeIntegralInRange<size_t>(0, sent.size() - 1)]};
                    LOCK(::cs_main);
                    if (const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(pick)}) {
                        parent = pick;
                        time = index->nTime + 1;
                    }
                }
                std::vector<CBlockHeader> headers;
                const size_t count{fdp.ConsumeIntegralInRange<size_t>(1, 6)};
                for (size_t i{0}; i < count; ++i) {
                    CBlockHeader header;
                    header.nVersion = 0x20000000 | BLOCK_EXTENSION_BIT;
                    header.hashPrevBlock = parent;
                    header.hashMerkleRoot = uint256{static_cast<uint8_t>(fdp.ConsumeIntegral<uint8_t>())};
                    header.nTime = time + static_cast<uint32_t>(i);
                    header.nBits = tip->nBits;
                    header.nNonce = ++serial;
                    header.ext.hashSections.SetNull();
                    headers.push_back(header);
                    parent = header.GetHash();
                    sent.push_back(parent);
                }
                BlockValidationState state;
                // A peer that never connected, or left, is refused too ("unchecked-budget").
                if (!chainman.ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state, nullptr, peer)) {
                    assert(state.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK || state.GetResult() == BlockValidationResult::BLOCK_MISSING_PREV);
                }
            });

        LOCK(::cs_main);
        chainman.CheckUnchecked();
        assert(chainman.m_unchecked.size() <= limits.total);
        assert(chainman.m_blockman.m_block_index.size() == base + chainman.m_unchecked.size());
        for (int64_t source{0}; source < 8; ++source) {
            assert(chainman.UncheckedHeld(source) <= limits.per_peer);
            if (!live.contains(source)) assert(chainman.UncheckedHeld(source) == 0);
        }
        assert(chainman.m_best_header && chainman.m_blockman.LookupBlockIndex(chainman.m_best_header->GetBlockHash()) == chainman.m_best_header);
    }

    // Every peer leaves: nothing is left of what they sent.
    {
        LOCK(::cs_main);
        for (const int64_t peer : live) chainman.UncheckedSourceGone(peer);
        assert(chainman.m_unchecked.empty());
        assert(chainman.m_blockman.m_block_index.size() == base);
        assert(chainman.m_best_header == chainman.ActiveChain().Tip());
    }
    sidechain::SetHooks(nullptr);
}
