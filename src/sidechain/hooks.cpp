// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/hooks.h>

#include <chain.h>
#include <consensus/validation.h>
#include <hash.h>
#include <pow.h>
#include <sidechain/mainchain.h>
#include <util/check.h>

#include <atomic>

namespace sidechain {

ChainHooks::~ChainHooks() = default;

BlockKind ChainHooks::Kind(const CBlockHeader&) const { return BlockKind::BMM; }

bool ChainHooks::CheckHeader(const CBlockHeader&, const CBlockIndex&, const ChainstateManager&, BlockValidationState& state) const
{
    // Only a sidechain that has kinds of its own says a block is of one, and checks it.
    return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, "bad-block-kind", "a block of a kind this chain does not have");
}

bool ChainHooks::CheckBlock(const CBlock&, BlockValidationState&) const { return true; }

uint256 ChainHooks::SectionsHash(std::span<const unsigned char> sections) const
{
    return (TaggedHash("sidechain/sections") << sections).GetSHA256();
}

std::vector<CTxOut> ChainHooks::MergeMinedOutputs(const StoreView&, const CBlockIndex&, int64_t) const { return {}; }

OtherExtension ChainHooks::CheckOtherExtension(const CBlockHeader&, const CBlockIndex&) const { return OtherExtension::INVALID; }

int64_t ChainHooks::MaxFutureBlockTime() const { return MAX_FUTURE_BLOCK_TIME; }

arith_uint256 ChainHooks::ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const
{
    return (prev ? prev->nChainWork : arith_uint256{0}) + GetBlockProof(header);
}

bool ChainHooks::IsWaitReason(const std::string& reject_reason) const { return reject_reason == "bmm-unknown"; }

std::optional<int64_t> ChainHooks::WaitingReady(const CBlockHeader& header, const Mainchain& record) const
{
    if (const auto height{record.CommittedHeight(header.GetHash())}) return *height;
    return std::nullopt;
}

std::optional<uint256> ChainHooks::CommitmentOf(const CBlockIndex& index) const { return index.GetBlockHash(); }

std::vector<uint256> ChainHooks::BlocksCommittedBy(const uint256& commitment) const { return {commitment}; }

bool ChainHooks::GetPegStep(const CBlock& block, const Mainchain& mainchain, std::optional<PegStep>& step, std::string& reject_reason) const
{
    // The block follows the mainchain up to the block before the one that committed to it.
    const auto bmm_height{mainchain.BmmHeight(block.GetHash())};
    if (!bmm_height) {
        reject_reason = "bmm-unknown";
        return false;
    }
    step = PegStep{.main_height = *bmm_height - 1, .nonce = block.hashPrevBlock, .max_withdrawal_height = std::nullopt, .peg_height = std::nullopt};
    return true;
}

bool ChainHooks::BeforePegStep(State&, const CBlock&, int, const PegStep&, const Consensus::SidechainParams&, std::vector<CTxOut>&, std::string&) const { return true; }

bool ChainHooks::ApplyTx(State&, const CTransaction&, int, bool, std::vector<CTxOut>&, std::string&) const { return true; }

bool ChainHooks::AfterBlock(State&, const CBlock&, int, const std::optional<PegStep>&, CAmount, const Consensus::SidechainParams&, std::string&) const { return true; }

bool ChainHooks::FollowerRound(Follower&) const { return false; }

void ChainHooks::StartServices(node::NodeContext&) const {}

void ChainHooks::StopServices(node::NodeContext&) const {}

bool ChainHooks::ProcessMessage(int64_t, const std::string&, DataStream&, std::string&) const { return false; }

namespace {
const ChainHooks g_template_hooks;
std::unique_ptr<ChainHooks> g_installed;
std::atomic<const ChainHooks*> g_hooks{&g_template_hooks};
} // namespace

const ChainHooks& Hooks() { return *g_hooks.load(std::memory_order_acquire); }

std::unique_ptr<ChainHooks> SetHooks(std::unique_ptr<ChainHooks> hooks)
{
    std::unique_ptr<ChainHooks> before{std::move(g_installed)};
    g_installed = std::move(hooks);
    const ChainHooks& now{g_installed ? *g_installed : g_template_hooks};
    SetBlockExtensions(now.BlockExtensions());
    g_hooks.store(&now, std::memory_order_release);
    return before;
}

bool CheckHeaderWork(const CBlockHeader& header, const Consensus::Params& params)
{
    if (Hooks().Kind(header) != BlockKind::BMM) return true;
    return CheckProofOfWork(header.GetHash(), header.nBits, params);
}

bool CheckSections(const CBlock& block)
{
    if (!block.IsExtended()) return true;
    const uint256 expected{block.vSections.empty() ? uint256{} : Hooks().SectionsHash(block.vSections)};
    return block.ext.hashSections == expected;
}

bool SameExtension(const CBlockHeader& header, const CBlockIndex& index)
{
    if (!header.IsExtended()) return !index.m_ext;
    return index.m_ext && *index.m_ext == header.ext;
}

} // namespace sidechain
