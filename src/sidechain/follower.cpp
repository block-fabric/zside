// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/follower.h>

#include <drivechain/db.h>

#include <chain.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <interfaces/mining.h>
#include <logging.h>
#include <node/context.h>
#include <node/warnings.h>
#include <pow.h>
#include <rpc/util.h>
#include <sidechain/mainchain.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <txmempool.h>
#include <node/blockstorage.h>
#include <sidechain/state.h>
#include <primitives/block.h>
#include <validation.h>

namespace sidechain {
namespace {
//! How often the mainchain node is asked for news.
constexpr auto POLL_INTERVAL{1s};
//! Number of mainchain blocks asked for at a time.
constexpr int BATCH{2000};

UniValue Params(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const auto& value : values) params.push_back(value);
    return params;
}

uint256 Hash256(const UniValue& value)
{
    const auto hash{uint256::FromHex(value.get_str())};
    if (!hash) throw std::runtime_error("the mainchain node sent something that is not a hash");
    return *hash;
}

MainBlock ParseMainBlock(const UniValue& obj)
{
    MainBlock block;
    block.hash = Hash256(obj["hash"]);
    if (obj.exists("previousblockhash")) block.prev_hash = Hash256(obj["previousblockhash"]);
    // The median time of the block and the ten before it, which no single miner can set ahead (the
    // time of the block itself can be two hours ahead); the time of the block from an older node.
    block.time = obj.exists("mediantime") ? obj["mediantime"].getInt<int64_t>() : obj["time"].getInt<int64_t>();
    if (obj.exists("bmm")) block.bmm = Hash256(obj["bmm"]);
    for (const UniValue& entry : obj["deposits"].getValues()) {
        MainDeposit deposit;
        deposit.destination = entry["destination"].get_str();
        deposit.amount = AmountFromValue(entry["amount"]);
        deposit.txid = Hash256(entry["txid"]);
        deposit.burn_index = entry["burnindex"].getInt<uint32_t>();
        if (entry.exists("bundle")) deposit.bundle = Hash256(entry["bundle"]);
        if (entry.exists("payouts")) {
            for (const UniValue& payout : entry["payouts"].getValues()) {
                const std::vector<unsigned char> script{ParseHex(payout["script"].get_str())};
                deposit.payouts.emplace_back(AmountFromValue(payout["amount"]), CScript{script.begin(), script.end()});
            }
        }
        block.deposits.push_back(std::move(deposit));
    }
    for (const UniValue& entry : obj["bundles"].getValues()) {
        block.bundles.push_back({Hash256(entry["hash"]), entry["paid"].get_bool()});
    }
    return block;
}
} // namespace

Follower::Follower(node::NodeContext& node, MainClient::Options options) : m_node{node}, m_client{std::move(options)} {}

Follower::~Follower() { Stop(); }

void Follower::Start()
{
    LOCK(m_mutex);
    m_stop = false;
    m_thread = std::thread{[this] {
        util::ThreadRename("mainchain");
        Run();
    }};
}

void Follower::Stop()
{
    std::thread thread;
    {
        LOCK(m_mutex);
        m_stop = true;
        thread.swap(m_thread);
    }
    m_wake.notify_all();
    if (thread.joinable()) thread.join();
}

Follower::Status Follower::GetStatus() const
{
    LOCK(m_mutex);
    return m_status;
}

bool Follower::UpdateRecord(bool may_drop)
{
    Mainchain& record{*Assert(m_node.chainman->m_mainchain)};
    const uint32_t slot{m_node.chainman->GetConsensus().sidechain.slot};
    const auto fetch{[&](int height, int count) {
        return m_client.Call("getsidechainevents", Params({static_cast<uint64_t>(slot), height, count}));
    }};

    bool changed{false};
    while (!m_stop) {
        const int height{record.Height()};
        // The block on record is asked for again, to learn whether the mainchain still has it.
        const UniValue events{fetch(std::max(height, 0), BATCH)};
        size_t first_new{0};
        if (height >= 0) {
            if (events.empty() || Hash256(events[0]["hash"]) != record.TipHash()) {
                // Finding where the chains part takes calls to the mainchain node, which a caller
                // holding the main lock should not wait for.
                if (!may_drop) break;
                // A mainchain node that is still syncing, or reindexing, has not dropped anything: it
                // has not got there yet. Dropping blocks on its word would drop the sidechain blocks
                // committed in them too.
                const UniValue info{m_client.Call("getblockchaininfo", UniValue{UniValue::VARR})};
                if (info["initialblockdownload"].isTrue()) {
                    throw std::runtime_error("The mainchain node is still syncing; waiting for it to catch up before following it");
                }
                // The mainchain dropped blocks on record: go back to the last one it still has, a
                // batch of blocks at a time.
                int common{height - 1};
                while (common >= 0) {
                    const int from{std::max(common - BATCH + 1, 0)};
                    const UniValue batch{fetch(from, common - from + 1)};
                    int found{-1};
                    for (int h{common}; h >= from; --h) {
                        const size_t i{static_cast<size_t>(h - from)};
                        if (i < batch.size() && Hash256(batch[i]["hash"]) == record.GetBlock(h)->hash) {
                            found = h;
                            break;
                        }
                    }
                    if (found >= 0) {
                        common = found;
                        break;
                    }
                    common = from - 1;
                }
                const std::vector<MainBlock> removed{record.Truncate(common)};
                LogInfo("The mainchain dropped %d blocks above height %d", removed.size(), common);
                LOCK(m_mutex);
                for (const MainBlock& block : removed) {
                    // A block the mainchain committed to twice keeps its first commitment, below the
                    // blocks dropped: dropping the second one changes nothing for it.
                    if (block.bmm && !record.CommittedHeight(*block.bmm)) m_uncommitted.push_back(*block.bmm);
                }
                changed = true;
                continue;
            }
            first_new = 1;
        }
        if (events.size() <= first_new) break;
        for (size_t i{first_new}; i < events.size(); ++i) {
            const MainBlock block{ParseMainBlock(events[i])};
            // Not the block that follows: the mainchain changed between two calls. Start over.
            if (!record.Append(block)) break;
            changed = true;
            if (block.bmm) {
                LOCK(m_mutex);
                m_committed.push_back(*block.bmm);
            }
        }
    }
    return changed;
}

void Follower::Poll()
{
    // Headers from peers can ask for this at any rate, while the node holds its main lock: at most
    // one call to the mainchain node every few seconds; a header it does not answer for waits.
    const auto now{NodeClock::now()};
    {
        LOCK(m_mutex);
        if (now < m_last_poll + POLL_SPACING) return;
        m_last_poll = now;
    }
    TRY_LOCK(m_sync_mutex, locked);
    if (!locked) return;
    try {
        if (UpdateRecord(/*may_drop=*/false)) {
            LOCK(m_mutex);
            m_woken = true;
            m_wake.notify_all();
        }
    } catch (const std::exception&) {
        // The thread of the follower will run into the same error and report it.
    }
}

bool Follower::Sync(std::string& error)
{
    AssertLockNotHeld(::cs_main);
    try {
        {
            LOCK(m_sync_mutex);
            UpdateRecord();
        }
        // Stopping cut the update short: the record may be partial, nothing to act on.
        if (m_stop) return false;
        {
            // One at a time: two callers acting on the same changes could otherwise invalidate a
            // block after the other found it committed again.
            LOCK(m_act_mutex);
            Act();
            CheckActiveChain();
        }
        SendBundle();
    } catch (const std::exception& e) {
        error = e.what();
        LOCK(m_mutex);
        if (m_status.connected || m_status.error != error) {
            LogWarning("%s", error);
            // Shown where the node shows its warnings, such as the status bar of the GUI.
            if (m_node.warnings) {
                m_node.warnings->Unset(node::Warning::MAINCHAIN_UNREACHABLE);
                m_node.warnings->Set(node::Warning::MAINCHAIN_UNREACHABLE, strprintf(_("This node cannot follow the mainchain, without which it cannot tell which new blocks are valid. %s"), error));
            }
        }
        m_status = {false, error};
        return false;
    }
    LOCK(m_mutex);
    if (!m_status.connected) {
        LogInfo("Following the mainchain node at %s:%u", m_client.GetOptions().host, m_client.GetOptions().port);
        if (m_node.warnings) m_node.warnings->Unset(node::Warning::MAINCHAIN_UNREACHABLE);
    }
    m_status = {true, {}};
    return true;
}

void Follower::Act()
{
    ChainstateManager& chainman{*m_node.chainman};
    std::vector<uint256> uncommitted, committed;
    std::vector<std::shared_ptr<const CBlock>> to_submit;
    {
        LOCK(m_mutex);
        uncommitted.swap(m_uncommitted);
        committed.swap(m_committed);
        const Mainchain& record{*chainman.m_mainchain};
        for (const uint256& hash : committed) {
            const auto it{m_candidates.find(hash)};
            if (it == m_candidates.end()) continue;
            to_submit.push_back(it->second.block);
            if (m_mining.enabled) ++m_mining.blocks;
        }
        // A candidate can only be committed to by the block after the one it was built on.
        std::erase_if(m_candidates, [&](const auto& entry) { return entry.second.main_prev_height < record.Height(); });
    }
    if (uncommitted.empty() && committed.empty()) return;

    // A block that lost its commitment is no longer valid, and neither is what was built on it.
    const Mainchain& record{*chainman.m_mainchain};
    // That includes a block whose commitment moved to another mainchain block in the same update:
    // what the mainchain did before its commitment may differ now, so it is checked again, from the
    // list of committed blocks below.
    for (const uint256& hash : uncommitted) {
        CBlockIndex* pindex{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(hash))};
        if (!pindex) continue;
        LogInfo("Block %s lost its commitment on the mainchain", hash.ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, pindex);
    }
    {
        LOCK(::cs_main);
        for (const uint256& hash : committed) {
            CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(hash)};
            if (pindex && (pindex->nStatus & BLOCK_FAILED_VALID)) {
                LogInfo("Block %s has a commitment on the mainchain again", hash.ToString());
                chainman.ActiveChainstate().ResetBlockFailureFlags(pindex);
                chainman.RecalculateBestHeader();
            }
        }
    }
    // Headers that came before the commitment to them was on record. With the
    // header accepted, the block is fetched from the peers that announced it.
    for (const uint256& hash : committed) {
        std::optional<CBlockHeader> header;
        {
            LOCK(::cs_main);
            if (const auto it{chainman.m_bmm_waiting.find(hash)}; it != chainman.m_bmm_waiting.end()) {
                header = it->second.header;
                chainman.m_bmm_waiting.erase(it);
            }
        }
        if (!header) continue;
        BlockValidationState state;
        chainman.ProcessNewBlockHeaders({{*header}}, /*min_pow_checked=*/true, state);
    }
    for (const auto& block : to_submit) {
        bool new_block{false};
        if (!chainman.ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) {
            LogWarning("The block %s that the mainchain committed to was not accepted", block->GetHash().ToString());
        }
    }
    BlockValidationState state;
    chainman.ActiveChainstate().ActivateBestChain(state);
}

void Follower::CheckActiveChain()
{
    if (WITH_LOCK(m_mutex, return m_chain_checked)) return;
    ChainstateManager& chainman{*m_node.chainman};
    const Mainchain& record{*Assert(chainman.m_mainchain)};
    // Blocks that lost their commitment are dropped as soon as the record changes, but a node that
    // stopped in between, or that was down meanwhile, starts with them on its active chain. The
    // mainchain commits to the blocks of a chain in order: the uncommitted ones are at the top.
    CBlockIndex* lowest{nullptr};
    CBlockIndex* moved{nullptr};
    {
        LOCK(::cs_main);
        for (CBlockIndex* pindex{chainman.ActiveChain().Tip()}; pindex && pindex->nHeight > 0; pindex = pindex->pprev) {
            if (record.CommittedHeight(pindex->GetBlockHash())) break;
            lowest = pindex;
        }
        // Each block was checked against what the mainchain did up to the block before its
        // commitment. Commitments that moved while the node was down -- in a reorg of the mainchain
        // -- are found from the tip down, as far as such a reorg could reach; the lowest block whose
        // commitment moved is checked again, with all above it.
        if (!lowest) {
            int main_height{chainman.ActiveChainstate().m_scdb.m_side.MainHeight()};
            for (CBlockIndex* pindex{chainman.ActiveChain().Tip()}; pindex && pindex->nHeight > 0; pindex = pindex->pprev) {
                const auto bmm_height{record.CommittedHeight(pindex->GetBlockHash())};
                if (!bmm_height) break;
                if (*bmm_height - 1 != main_height) moved = pindex;
                if (*bmm_height < record.Height() - MOVED_CHECK_DEPTH) break;
                // What the mainchain had done before this block is what it had done after its parent.
                drivechain::BlockUndo undo;
                if (!chainman.m_blockman.m_drivechain_db->ReadBlockUndo(pindex->GetBlockHash(), undo)) break;
                main_height = undo.side.main_height;
            }
        }
    }
    if (lowest) {
        LogInfo("Block %s and the blocks after it have no commitment on the mainchain", lowest->GetBlockHash().ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, lowest);
    } else if (moved) {
        LogInfo("The commitment of block %s moved on the mainchain; checking it and the blocks after it again", moved->GetBlockHash().ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, moved);
        {
            LOCK(::cs_main);
            chainman.ActiveChainstate().ResetBlockFailureFlags(moved);
            chainman.RecalculateBestHeader();
        }
        chainman.ActiveChainstate().ActivateBestChain(state);
    }
    LOCK(m_mutex);
    m_chain_checked = true;
}

void Follower::SendBundle()
{
    ChainstateManager& chainman{*m_node.chainman};
    std::optional<CMutableTransaction> tx;
    uint256 hash;
    {
        LOCK(::cs_main);
        const State& side{chainman.ActiveChainstate().m_scdb.m_side};
        if (!side.Bundle()) return;
        hash = side.Bundle()->hash;
        tx = side.BundleTx();
    }
    if (!tx) return;
    const uint64_t slot{chainman.GetConsensus().sidechain.slot};
    if (WITH_LOCK(m_mutex, return m_bundle_sent != hash)) {
        // The mainchain node proposes the bundle in the blocks it mines; others learn its hash from those.
        // That it refuses the bundle is no reason to stop following the mainchain: it is logged, and tried
        // again with the next block.
        try {
            m_client.Call("receivewithdrawalbundle", Params({slot, EncodeHexTx(CTransaction{*tx})}));
        } catch (const std::exception& e) {
            if (WITH_LOCK(m_mutex, return m_bundle_refused != hash)) {
                LogWarning("The mainchain node did not take the withdrawal bundle %s: %s", hash.ToString(), e.what());
                LOCK(m_mutex);
                m_bundle_refused = hash;
            }
            return;
        }
        LogInfo("Gave the mainchain node the withdrawal bundle %s", hash.ToString());
        LOCK(m_mutex);
        m_bundle_sent = hash;
    }
    // Once the bundle has the work score, the mainchain node broadcasts the
    // withdrawal that pays it out, and any miner can mine it: one try per
    // mainchain block, since the treasury output it spends can change.
    const int main_height{Assert(chainman.m_mainchain)->Height()};
    if (WITH_LOCK(m_mutex, return m_payout_tried == std::make_pair(hash, main_height))) return;
    try {
        const UniValue sent{m_client.Call("sendwithdrawalbundle", Params({slot, hash.GetHex()}))};
        if (sent.isObject() && sent.find_value("sent").isTrue() && sent.find_value("txid").isStr()) {
            LogInfo("The mainchain node broadcast the withdrawal %s paying out the bundle %s", sent.find_value("txid").get_str(), hash.ToString());
        }
    } catch (const std::exception& e) {
        // A mainchain node too old to broadcast withdrawals pays them out in the blocks it mines.
        LogDebug(BCLog::NET, "The mainchain node did not broadcast the withdrawal of bundle %s: %s", hash.ToString(), e.what());
    }
    LOCK(m_mutex);
    m_payout_tried = {hash, main_height};
}

std::optional<Follower::Candidate> Follower::CreateCandidate(const CScript& coinbase_script, std::string& error)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Mainchain& record{*Assert(chainman.m_mainchain)};
    if (!Sync(error)) return std::nullopt;
    for (int attempt{0}; attempt < 10 && !m_stop; ++attempt) {
        Candidate candidate;
        candidate.main_prev_height = record.Height();
        candidate.main_prev_hash = record.TipHash();
        if (candidate.main_prev_height < 0) {
            error = "The mainchain has no blocks on record yet";
            return std::nullopt;
        }
        std::unique_ptr<interfaces::BlockTemplate> block_template;
        try {
            block_template = Assert(m_node.mining)->createNewBlock({.coinbase_output_script = coinbase_script}, /*cooldown=*/false);
        } catch (const std::exception& e) {
            error = e.what();
            return std::nullopt;
        }
        if (!block_template) {
            error = "The node is shutting down";
            return std::nullopt;
        }
        // The block was built for the mainchain as it was on record; if that moved, build another.
        if (record.TipHash() != candidate.main_prev_hash) continue;

        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        // The chain is secured by the work of the mainchain; its own proof of work is a formality.
        while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
        candidate.block = std::make_shared<const CBlock>(std::move(block));
        LOCK(m_mutex);
        m_candidates[candidate.block->GetHash()] = candidate;
        return candidate;
    }
    error = "The mainchain keeps moving";
    return std::nullopt;
}

std::optional<std::string> Follower::RequestCommitment(const Candidate& candidate, CAmount amount, std::string& error) const
{
    try {
        const UniValue result{m_client.Call("createbmmrequest", Params({static_cast<uint64_t>(m_node.chainman->GetConsensus().sidechain.slot), candidate.block->GetHash().GetHex(), ValueFromAmount(amount)}), /*wallet=*/true)};
        // The request names the mainchain block it has to follow; it has to be the one the candidate was built on.
        if (Hash256(result["prevblockhash"]) != candidate.main_prev_hash) {
            error = "The mainchain moved while the request was made";
            return std::nullopt;
        }
        return result["txid"].get_str();
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

void Follower::SetMining(bool enabled, const CScript& coinbase_script, bool always, CAmount amount)
{
    LOCK(m_mutex);
    m_mining.enabled = enabled;
    m_mining.coinbase_script = coinbase_script;
    m_mining.always = always;
    m_mining.amount = amount;
    m_mining.idle = false;
    m_mining.error.clear();
    m_mining_requested_for.SetNull();
    ++m_mining_settings;
    m_woken = true;
    m_wake.notify_all();
}

void Follower::DropCandidate(const uint256& hash)
{
    LOCK(m_mutex);
    m_candidates.erase(hash);
}

Follower::Mining Follower::GetMining() const
{
    LOCK(m_mutex);
    return m_mining;
}

void Follower::Mine()
{
    Mining mining;
    uint64_t settings;
    {
        LOCK(m_mutex);
        mining = m_mining;
        settings = m_mining_settings;
        if (!mining.enabled) return;
        // One request per mainchain block.
        if (m_mining_requested_for == m_node.chainman->m_mainchain->TipHash() && !m_mining_requested_for.IsNull()) return;
    }
    // Mining may be set otherwise while this runs; what was decided for the old settings then says nothing.
    const auto wait{[this, settings] {
        LOCK(m_mutex);
        if (m_mining_settings == settings) m_mining.idle = true;
    }};
    // Without transactions there are no fees, and a block without fees is not worth what the mainchain is paid for it.
    if (!mining.always && (!m_node.mempool || m_node.mempool->size() == 0)) return wait();

    std::string error;
    const auto candidate{CreateCandidate(mining.coinbase_script, error)};
    CAmount offer{mining.amount};
    CAmount fees{0};
    if (candidate) {
        // The first output of the coinbase is the fees of the block; the others pay deposits and the like.
        fees = candidate->block->vtx[0]->vout[0].nValue;
        if (!mining.always) {
            offer = Offer(fees);
            if (offer < MIN_OFFER) {
                DropCandidate(candidate->block->GetHash());
                return wait();
            }
        }
    }
    const auto request{candidate ? RequestCommitment(*candidate, offer, error) : std::nullopt};
    if (candidate && !request && error.starts_with("Outbid:")) {
        // Another node of this chain bid more for its own block: the auction working, not a failure.
        // Nothing more is asked for until the next mainchain block.
        DropCandidate(candidate->block->GetHash());
        LOCK(m_mutex);
        if (m_mining_settings != settings) return;
        ++m_mining.outbid;
        m_mining_requested_for = candidate->main_prev_hash;
        m_mining.error.clear();
        LogDebug(BCLog::NET, "Merged mining: %s\n", error);
        return;
    }
    if (candidate && request) {
        LOCK(m_mutex);
        ++m_mining.requests;
        if (m_mining_settings != settings) return;
        m_mining_requested_for = candidate->main_prev_hash;
        m_mining.idle = false;
        m_mining.last_fees = fees;
        m_mining.last_offer = offer;
        m_mining.error.clear();
        return;
    }
    LOCK(m_mutex);
    if (m_mining.error != error) LogWarning("Merged mining: %s", error);
    m_mining.error = error;
}

void Follower::Run()
{
    while (!m_stop) {
        std::string error;
        if (Sync(error)) Mine();
        WAIT_LOCK(m_mutex, lock);
        m_wake.wait_for(lock, POLL_INTERVAL, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_stop.load() || m_woken; });
        m_woken = false;
    }
}

} // namespace sidechain
