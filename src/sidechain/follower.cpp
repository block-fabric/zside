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
#include <net_processing.h>
#include <node/context.h>
#include <node/warnings.h>
#include <pow.h>
#include <rpc/util.h>
#include <streams.h>
#include <util/strencodings.h>
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

/** An amount the mainchain node sent. AmountFromValue throws a UniValue, which nothing on the way
 * up catches: here it becomes a bad answer, as any other. */
CAmount ParseAmount(const UniValue& value)
{
    try {
        return AmountFromValue(value);
    } catch (const UniValue& e) {
        throw std::runtime_error(strprintf("the mainchain node sent something that is not an amount (%s)", e.find_value("message").getValStr()));
    }
}

/** The bundles of this sidechain pending after a mainchain block, with their scores. A node that does
 * not say is too old to follow: from SidechainParams::audit2_height they decide whether refunds wait. */
std::vector<MainPendingBundle> ParsePending(const UniValue& obj)
{
    if (!obj.exists("pending")) {
        throw std::runtime_error("The mainchain node is too old: it does not report the withdrawal bundles pending after blocks, with their scores. Upgrade Chains");
    }
    std::vector<MainPendingBundle> pending;
    for (const UniValue& entry : obj["pending"].getValues()) pending.push_back({Hash256(entry["hash"]), entry["score"].getInt<uint32_t>()});
    return pending;
}

/** The bundles a mainchain block proposed. A node that does not say is too old to follow: taking
 * its silence for "none" would let withdrawals in a pending bundle be refunded. */
std::vector<uint256> ParseProposed(const UniValue& obj)
{
    if (!obj.exists("proposed")) {
        throw std::runtime_error("The mainchain node is too old: it does not report the withdrawal bundles that blocks propose. Upgrade Chains");
    }
    std::vector<uint256> proposed;
    for (const UniValue& hash : obj["proposed"].getValues()) proposed.push_back(Hash256(hash));
    return proposed;
}

MainBlock ParseMainBlock(const UniValue& obj)
{
    MainBlock block;
    block.hash = Hash256(obj["hash"]);
    if (obj.exists("previousblockhash")) block.prev_hash = Hash256(obj["previousblockhash"]);
    // The median time of the block and the ten before it, which no single miner can set ahead (the
    // time of the block itself can be two hours ahead). Rules of sidechains depend on it: a node that
    // does not say is too old to follow, as nodes that took another time would disagree.
    if (!obj.exists("mediantime")) {
        throw std::runtime_error("The mainchain node is too old: it does not report the median time of blocks. Upgrade Chains");
    }
    block.time = obj["mediantime"].getInt<int64_t>();
    if (obj.exists("bmm")) block.bmm = Hash256(obj["bmm"]);
    for (const UniValue& entry : obj["deposits"].getValues()) {
        MainDeposit deposit;
        deposit.destination = entry["destination"].get_str();
        deposit.amount = ParseAmount(entry["amount"]);
        deposit.txid = Hash256(entry["txid"]);
        deposit.burn_index = entry["burnindex"].getInt<uint32_t>();
        if (entry.exists("bundle")) deposit.bundle = Hash256(entry["bundle"]);
        if (entry.exists("payouts")) {
            for (const UniValue& payout : entry["payouts"].getValues()) {
                const std::vector<unsigned char> script{ParseHex(payout["script"].get_str())};
                deposit.payouts.emplace_back(ParseAmount(payout["amount"]), CScript{script.begin(), script.end()});
            }
        }
        block.deposits.push_back(std::move(deposit));
    }
    for (const UniValue& entry : obj["bundles"].getValues()) {
        block.bundles.push_back({Hash256(entry["hash"]), entry["paid"].get_bool()});
    }
    block.proposed = ParseProposed(obj);
    block.pending = ParsePending(obj);
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
    // Before blocks on record are dropped, which cannot be taken back, the slot is checked to hold the
    // sidechain this node follows (after the mainchain node answered, so that what it answered is no
    // newer than what the check sees). Blocks added are checked after (Sync), before anything acts on
    // them: a node that finds another sidechain stops, and asking first would hold up every new block.
    bool slot_checked{false};
    const auto before_change{[&] {
        if (slot_checked) return;
        CheckSlot(/*record_changed=*/true);
        slot_checked = true;
    }};
    // Before blocks are dropped: the mainchain node has to be on the chain of the record (its first
    // block the same), and a drop that takes more commitments than a reorg of this chain can undo
    // waits for the operator (syncmainchain with allowdeepreorg): a wrong or lying mainchain node
    // could otherwise make this node drop its chain.
    const auto before_drop{[&](int keep) {
        const UniValue first{fetch(0, 1)};
        const auto genesis{record.GetBlock(0)};
        if (first.empty() || !genesis || Hash256(first[0]["hash"]) != genesis->hash) {
            throw std::runtime_error(strprintf("The mainchain node is on another chain than the one on record (its first block is %s, the record's %s); "
                                               "check -mainchainrpcconnect and -mainchainrpcport",
                                               first.empty() ? "missing" : first[0]["hash"].getValStr(), genesis ? genesis->hash.ToString() : "missing"));
        }
        int commitments{0};
        for (int h{keep + 1}; h <= record.Height(); ++h) {
            if (record.BmmAt(h)) ++commitments;
        }
        if (commitments > DRIVECHAIN_UNDO_DEPTH && !m_allow_deep_reorg) {
            throw std::runtime_error(strprintf("The mainchain node no longer has the %d blocks on record above height %d, with %d commitments to blocks of this chain: "
                                               "more than this node can take back (%d). If the mainchain really did reorganise this deep, call syncmainchain true",
                                               record.Height() - keep, keep, commitments, DRIVECHAIN_UNDO_DEPTH));
        }
        before_change();
        if (commitments > DRIVECHAIN_UNDO_DEPTH) {
            LogWarning("Dropping %d commitments of the mainchain record, as the operator allowed", commitments);
            m_allow_deep_reorg = false;
        }
    }};
    // A record written before it kept proposed bundles gets them, block by block, before anything
    // else. Blocks are only filled in, never dropped: no commitment goes missing meanwhile. A block
    // the mainchain no longer has stops it; the loop below then drops it and those above it, and
    // fetches what replaced them, proposals included.
    if (may_drop && record.NeedsBackfill()) {
        LogInfo("Filling in the withdrawal bundles proposed in the %d mainchain blocks on record", record.Height() + 1);
        int filled{-1};
        std::optional<int> moved;
        for (int from{0}; from <= record.Height() && !moved; from += BATCH) {
            if (m_stop) return changed;
            const UniValue batch{fetch(from, BATCH)};
            for (size_t i{0}; i < batch.size() && from + static_cast<int>(i) <= record.Height(); ++i) {
                const int h{from + static_cast<int>(i)};
                if (!record.Backfill(h, Hash256(batch[i]["hash"]), ParseProposed(batch[i]), ParsePending(batch[i]))) {
                    moved = h;
                    break;
                }
                filled = h;
            }
            // A batch cut short: the mainchain node does not have the blocks yet.
            if (!moved && filled < std::min(from + BATCH - 1, record.Height())) break;
        }
        if (moved) {
            // The mainchain left the record at this height: what is above goes, and comes back, proposals
            // included, from the mainchain as it is now.
            before_drop(*moved - 1);
            const std::vector<MainBlock> removed{record.Truncate(*moved - 1)};
            LOCK(m_mutex);
            for (const MainBlock& block : removed) {
                if (block.bmm && !record.CommittedHeight(*block.bmm)) m_uncommitted.push_back(*block.bmm);
            }
            changed = true;
        } else if (filled < record.Height()) {
            throw std::runtime_error("The mainchain node does not have all the blocks on record yet (is it still syncing?); waiting for it before going on");
        }
        record.BackfillDone();
        // No block of the active chain acted on what the record missed: nothing to check again. (So
        // it is for a chainstate built anew, whose blocks waited for the record to be filled in.)
        const Consensus::SidechainParams& params{m_node.chainman->GetConsensus().sidechain};
        if (WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height()) < std::min(params.single_bundle_height, params.audit2_height)) {
            record.RecheckDone();
        }
    }
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
                // No block at the height of the record: a mainchain node behind it (restarted after
                // losing its last blocks, or another node) is on the same chain if its own tip is the
                // record's block at that height. It has dropped nothing; it catches up.
                if (events.empty()) {
                    const int node_height{m_client.Call("getblockcount", UniValue{UniValue::VARR}).getInt<int>()};
                    if (node_height >= 0 && node_height < height) {
                        const UniValue tip{fetch(node_height, 1)};
                        const auto on_record{record.GetBlock(node_height)};
                        if (!tip.empty() && on_record && Hash256(tip[0]["hash"]) == on_record->hash) {
                            throw std::runtime_error(strprintf("The mainchain node is behind the record (at height %d, the record at %d); waiting for it to catch up",
                                                               node_height, height));
                        }
                    }
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
                before_drop(common);
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
        // All of the answer is read first: one that cannot be read changes nothing.
        std::vector<MainBlock> blocks;
        for (size_t i{first_new}; i < events.size(); ++i) blocks.push_back(ParseMainBlock(events[i]));
        for (size_t i{first_new}; i < events.size(); ++i) {
            const MainBlock& block{blocks[i - first_new]};
            if (!record.Append(block)) {
                // Not the block that follows. Blocks of the answer taken before it: the next call starts
                // after them and finds out. None: the answer starts at the tip on record, then does not
                // follow it; asking again at once would get the same answer, for ever.
                if (i == first_new) {
                    throw std::runtime_error(strprintf("The mainchain node sent blocks that do not follow each other (at height %d); asking again shortly", std::max(height, 0) + static_cast<int>(i)));
                }
                break;
            }
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
    // Headers from peers can ask for this at any rate: the follower is woken at most every few
    // seconds; a header whose commitment is not on record yet waits for it.
    const auto now{NodeClock::now()};
    {
        LOCK(m_mutex);
        if (now < m_last_poll + POLL_SPACING) return;
        m_last_poll = now;
    }
    // The caller holds the main lock: no call to the mainchain node here, which could take as long as
    // its timeout. The thread of the follower updates the record, and takes up the headers that wait
    // for a commitment once it is on record.
    LOCK(m_mutex);
    m_woken = true;
    m_wake.notify_all();
}

bool Follower::Sync(std::string& error)
{
    AssertLockNotHeld(::cs_main);
    bool failed{false};
    try {
        bool changed;
        {
            LOCK(m_sync_mutex);
            changed = UpdateRecord();
        }
        // Stopping cut the update short: the record may be partial, nothing to act on.
        if (m_stop) return false;
        // Blocks added: checked before anything acts on them (UpdateRecord checks before it drops any).
        CheckSlot(changed);
        {
            // One at a time: two callers acting on the same changes could otherwise invalidate a
            // block after the other found it committed again.
            LOCK(m_act_mutex);
            Act();
            CheckActiveChain();
        }
        SendBundle();
    } catch (const std::exception& e) {
        failed = true;
        error = e.what();
    } catch (const UniValue& e) {
        // An RPC error object thrown on the way (as AmountFromValue does): a bad answer like any other.
        failed = true;
        error = strprintf("Unexpected answer from the mainchain node: %s", e.find_value("message").getValStr());
    }
    if (failed) {
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

void Follower::CheckSlot(bool record_changed)
{
    // Another sidechain can take the slot only in a new mainchain block.
    if (!record_changed && WITH_LOCK(m_mutex, return m_slot_checked)) return;
    const Consensus::SidechainParams& params{m_node.chainman->GetConsensus().sidechain};
    Mainchain& record{*Assert(m_node.chainman->m_mainchain)};
    const auto known{record.GetSlotIdentity()};
    UniValue slot;
    try {
        slot = m_client.Call("getsidechain", Params({static_cast<uint64_t>(params.slot)}));
    } catch (const MainClientError& e) {
        if (!e.rpc_error) throw;
        // No sidechain in the slot: before this one activated there is nothing to compare, and
        // nothing of this chain on the mainchain either.
        if (known || params.main_activation_height > 0) {
            throw std::runtime_error(strprintf("The mainchain has no sidechain in slot %u (%s); waiting for it", params.slot, e.what()));
        }
        LOCK(m_mutex);
        m_slot_checked = true;
        return;
    }
    const SlotIdentity seen{slot["activationheight"].getInt<int32_t>(), Hash256(slot["proposalhash"])};
    std::string followed;
    if (params.main_activation_height > 0 && seen.activation_height != params.main_activation_height) {
        followed = strprintf("this chain is the one activated at height %d", params.main_activation_height);
    } else if (known && *known != seen) {
        followed = strprintf("this node followed the one activated at height %d by proposal %s", known->activation_height, known->proposal_hash.ToString());
    }
    if (!followed.empty()) {
        // Following on would credit this chain with what the mainchain does for another sidechain.
        const std::string message{strprintf("The sidechain in slot %u of the mainchain is another one: activated at height %d by proposal %s, while %s. "
                                            "This node stops rather than follow another sidechain.",
                                            params.slot, seen.activation_height, seen.proposal_hash.ToString(), followed)};
        m_stop = true;
        m_node.chainman->GetNotifications().fatalError(Untranslated(message));
        throw std::runtime_error(message);
    }
    if (!known) record.SetSlotIdentity(seen);
    LOCK(m_mutex);
    m_slot_checked = true;
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
    // Headers that came before the commitment to them was on record (see below).
    const auto waiting_ready{[&] {
        LOCK(::cs_main);
        const Mainchain& record{*chainman.m_mainchain};
        return std::any_of(chainman.m_bmm_waiting.begin(), chainman.m_bmm_waiting.end(), [&](const auto& entry) { return record.CommittedHeight(entry.first).has_value(); });
    }};
    if (uncommitted.empty() && committed.empty() && !waiting_ready()) return;

    // A block that lost its commitment is no longer valid, and neither is what was built on it.
    // That includes a block whose commitment moved to another mainchain block in the same update:
    // what the mainchain did before its commitment may differ now, so it is checked again, from the
    // list of committed blocks below.
    for (const uint256& hash : uncommitted) {
        CBlockIndex* pindex{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(hash))};
        if (!pindex) continue;
        LogInfo("Block %s lost its commitment on the mainchain", hash.ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, pindex, /*by_record=*/true);
    }
    {
        LOCK(::cs_main);
        for (const uint256& hash : committed) {
            CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(hash)};
            // Only a failure that depended on the record: not that of a block that broke a rule
            // whatever the mainchain did, nor one the operator marked invalid.
            if (pindex && chainman.ActiveChainstate().ReconsiderRecordFailure(pindex)) {
                LogInfo("Block %s has a commitment on the mainchain again", hash.ToString());
            }
        }
    }
    // Headers that came before the commitment to them was on record. With the header accepted, the
    // block is fetched from the peers that announced it. Every one whose commitment is on record now,
    // not only those of the blocks just added: a header that came while the record was being updated
    // would otherwise wait for ever.
    // In the order of their commitments, which is that of the chain: each header follows the one before.
    std::map<int, std::pair<CBlockHeader, int64_t>> headers;
    {
        LOCK(::cs_main);
        const Mainchain& record{*chainman.m_mainchain};
        for (auto it{chainman.m_bmm_waiting.begin()}; it != chainman.m_bmm_waiting.end();) {
            if (const auto height{record.CommittedHeight(it->first)}) {
                headers.emplace(*height, std::make_pair(it->second.header, it->second.peer));
                it = chainman.m_bmm_waiting.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& [_, entry] : headers) {
        const auto& [header, peer]{entry};
        BlockValidationState state;
        const CBlockIndex* pindex{nullptr};
        if (!chainman.ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state, &pindex) || !pindex || peer < 0 || !m_node.peerman) continue;
        // The block, from the peer that announced it: a header with less work than what the peer
        // announced before (blocks the record made invalid) would not be fetched otherwise.
        if (!WITH_LOCK(::cs_main, return pindex->nStatus & BLOCK_HAVE_DATA)) {
            if (const auto fetched{m_node.peerman->FetchBlock(peer, *pindex)}; !fetched) {
                LogDebug(BCLog::NET, "Block %s, now committed to, not fetched from peer=%d: %s", header.GetHash().ToString(), peer, fetched.error());
            }
        }
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
    Mainchain& record{*Assert(chainman.m_mainchain)};
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
            int main_height{chainman.ActiveChainstate().SideState().MainHeight()};
            for (CBlockIndex* pindex{chainman.ActiveChain().Tip()}; pindex && pindex->nHeight > 0; pindex = pindex->pprev) {
                const auto bmm_height{record.CommittedHeight(pindex->GetBlockHash())};
                if (!bmm_height) break;
                if (*bmm_height - 1 != main_height) moved = pindex;
                if (*bmm_height < record.Height() - MOVED_CHECK_DEPTH) break;
                // What the mainchain had done before this block is what it had done after its parent.
                drivechain::BlockUndo undo;
                if (!chainman.m_blockman.m_drivechain_db->ReadBlockUndo(pindex->GetBlockHash(), undo)) break;
                main_height = State::MainHeightBefore(undo.side, main_height);
            }
        }
    }
    // Blocks connected while the record missed the bundles that mainchain blocks proposed were checked
    // as if none was pending (State::MainPending): the lowest that started a bundle or refunded a
    // withdrawal while one was pending is checked again, with those above it.
    const bool recheck{!lowest && record.RecheckPending()};
    // Whether the blocks to be checked again were: the recheck is done only then.
    bool rechecked{recheck};
    std::optional<std::string> too_deep;
    if (recheck) {
        LOCK(::cs_main);
        // The same blocks as those that wait for the record to be filled in (WaitsForBackfill).
        const Consensus::SidechainParams& params{chainman.GetConsensus().sidechain};
        const int from{std::max(1, std::min(params.single_bundle_height, params.audit2_height))};
        // From the bottom up, up to the first block that acted while a bundle was pending: the
        // lowest to check again, and the scan stops there.
        CBlockIndex* first{nullptr};
        for (int height{from}; height <= chainman.ActiveChain().Height() && !first; ++height) {
            CBlockIndex* pindex{chainman.ActiveChain()[height]};
            // A block follows the mainchain up to the block before its commitment, then checks.
            const auto bmm_height{record.CommittedHeight(pindex->GetBlockHash())};
            if (!bmm_height || !record.BundlePending(*bmm_height - 1)) continue;
            // A pruned block cannot be checked again: it is left as it is.
            CBlock block;
            if (!chainman.m_blockman.ReadBlock(block, *pindex)) continue;
            const bool acts{std::any_of(block.vtx.begin(), block.vtx.end(), [](const CTransactionRef& tx) {
                return std::any_of(tx->vout.begin(), tx->vout.end(), [&](const CTxOut& out) {
                    return tx->IsCoinBase() ? ParseBundleCommitScript(out.scriptPubKey).has_value() : ParseRefundScript(out.scriptPubKey).has_value();
                });
            })};
            if (acts) first = pindex;
        }
        if (first) {
            // Checking it again takes it back first, which needs the undo data of the sidechain state
            // from the tip down to it: kept only for the last DRIVECHAIN_UNDO_DEPTH blocks. Deeper,
            // the chainstate has to be built anew; the recheck stays to be done until then, and the
            // node does not go on with blocks a node synced from scratch would refuse.
            drivechain::BlockUndo undo;
            if (!chainman.m_blockman.m_drivechain_db->ReadBlockUndo(first->GetBlockHash(), undo)) {
                too_deep = strprintf("The record of the mainchain is complete again, and block %s at height %d, which acted while a "
                                     "withdrawal bundle was pending on the mainchain, has to be checked again; it is too deep to be taken "
                                     "back in place (the undo data is kept for the last %d blocks). Restart with -reindex-chainstate.",
                                     first->GetBlockHash().ToString(), first->nHeight, DRIVECHAIN_UNDO_DEPTH);
            } else {
                LogInfo("The record of the mainchain is complete again: checking block %s and the blocks after it again", first->GetBlockHash().ToString());
                if (!moved || first->nHeight < moved->nHeight) moved = first;
            }
        }
    }
    if (too_deep) {
        m_stop = true;
        chainman.GetNotifications().fatalError(Untranslated(*too_deep));
        throw std::runtime_error(*too_deep);
    }
    if (lowest) {
        LogInfo("Block %s and the blocks after it have no commitment on the mainchain", lowest->GetBlockHash().ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, lowest, /*by_record=*/true);
    } else if (moved) {
        LogInfo("The commitment of block %s moved on the mainchain, or the record before it changed; checking it and the blocks after it again", moved->GetBlockHash().ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, moved, /*by_record=*/true);
        // Taken off the active chain, or not (interrupted, a block whose undo data is missing): what
        // was marked failed on the way is taken back either way, and the chain connected again.
        const bool taken_back{WITH_LOCK(::cs_main, return !chainman.ActiveChain().Contains(*moved))};
        if (!taken_back) {
            LogWarning("Block %s could not be taken back to be checked again; trying again later", moved->GetBlockHash().ToString());
            rechecked = false;
        }
        WITH_LOCK(::cs_main, chainman.ActiveChainstate().ReconsiderRecordFailure(moved));
        chainman.ActiveChainstate().ActivateBestChain(state);
    }
    if (rechecked) record.RecheckDone();
    // Blocks marked failed against the record, whose commitment the record has now: a node that
    // stopped between learning of the commitment and acting on it would otherwise never take them
    // again. Each has a commitment on the mainchain, which costs a fee: there cannot be many.
    size_t again{0};
    {
        LOCK(::cs_main);
        std::vector<CBlockIndex*> failed;
        for (auto& [hash, index] : chainman.m_blockman.m_block_index) {
            if ((index.nStatus & BLOCK_FAILED_VALID) && record.CommittedHeight(hash)) failed.push_back(&index);
        }
        for (CBlockIndex* pindex : failed) again += chainman.ActiveChainstate().ReconsiderRecordFailure(pindex);
    }
    if (again > 0) {
        LogInfo("Checking again %d blocks that failed against the record and have a commitment on the mainchain", again);
        BlockValidationState state;
        chainman.ActiveChainstate().ActivateBestChain(state);
    }
    // A recheck left for after the blocks without commitment went: on the next round.
    const bool done{!record.RecheckPending()};
    LOCK(m_mutex);
    m_chain_checked = done;
}

bool Follower::CatchingUp() const
{
    ChainstateManager& chainman{*m_node.chainman};
    if (chainman.IsInitialBlockDownload()) return true;
    const Mainchain& record{*Assert(chainman.m_mainchain)};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    const int tip_commitment{tip->nHeight == 0 ? -1 : record.CommittedHeight(tip->GetBlockHash()).value_or(-1)};
    // The last commitment on record, if it is to another block than the tip, well after the tip's: to
    // one this node has yet to connect, or does not know (a block it has not had yet). Not to a block
    // known to have failed, or to be on a branch with no more work.
    const int top{record.Height()};
    for (int height{top}; height > tip_commitment + CATCH_UP_MARGIN && height > top - CATCH_UP_WINDOW; --height) {
        const auto bmm{record.BmmAt(height)};
        if (!bmm) continue;
        const CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(*bmm)};
        if (!pindex) {
            // A block not known yet counts only if a peer announced its header, which waits for the
            // commitment: anyone can have the mainchain commit to a hash that is no block at all.
            if (chainman.m_bmm_waiting.contains(*bmm)) return true;
            continue;
        }
        return !(pindex->nStatus & BLOCK_FAILED_VALID) && !chainman.ActiveChain().Contains(*pindex) && pindex->nChainWork > tip->nChainWork;
    }
    return false;
}

void Follower::SendBundle()
{
    ChainstateManager& chainman{*m_node.chainman};
    // A node still catching up knows the bundle of its chain as it was: it says nothing on it rather
    // than something the mainchain would vote by. What it said last stands meanwhile.
    if (CatchingUp()) return;
    std::optional<CMutableTransaction> tx;
    uint256 hash;
    int side_main_height{-1};
    {
        LOCK(::cs_main);
        const State side{chainman.ActiveChainstate().SideState()};
        if (const auto pending{side.Bundle()}) {
            hash = pending->hash;
            tx = side.BundleTx();
        }
        side_main_height = side.MainHeight();
    }
    // The record may already say what the next block of this chain will learn: that the bundle was
    // closed, or that the mainchain paid another bundle of the slot, which can hold the same
    // withdrawals. The bundle is not vouched for meanwhile: paid after the other, it would pay them a
    // second time, and a chain without a new block for a while would otherwise keep it upvoted.
    if (!hash.IsNull()) {
        const Mainchain& record{*Assert(chainman.m_mainchain)};
        for (int h{side_main_height + 1}; h <= record.Height() && !hash.IsNull(); ++h) {
            const auto block{record.GetBlock(h)};
            if (!block) break;
            for (const MainBundleEvent& event : block->bundles) {
                if (event.hash == hash || event.paid) {
                    hash.SetNull();
                    tx.reset();
                    break;
                }
            }
        }
    }
    const uint64_t slot{chainman.GetConsensus().sidechain.slot};
    // Tell the mainchain node what this chain vouches for, when it changes: its miners upvote that
    // bundle, and downvote any other of this slot (one a reorg left behind) so that it fails.
    const auto vouch{[&](const uint256& bundle) {
        if (WITH_LOCK(m_mutex, return m_vouched == bundle)) return;
        try {
            if (bundle.IsNull()) {
                m_client.Call("vouchwithdrawalbundle", Params({slot}));
            } else {
                m_client.Call("vouchwithdrawalbundle", Params({slot, bundle.GetHex()}));
            }
        } catch (const std::exception& e) {
            if (WITH_LOCK(m_mutex, return !m_vouched.has_value() || !m_vouch_warned)) {
                LogWarning("The mainchain node did not take the word of this chain on its bundle (%s); it may be too old to vote by it", e.what());
                LOCK(m_mutex);
                m_vouch_warned = true;
            }
            return;
        }
        LOCK(m_mutex);
        m_vouched = bundle;
    }};
    if (hash.IsNull()) {
        vouch(uint256{});
        return;
    }
    if (!tx) return;
    if (WITH_LOCK(m_mutex, return m_bundle_sent != hash)) {
        // The mainchain node proposes the bundle in the blocks it mines; others learn its hash from those.
        // That it refuses the bundle is no reason to stop following the mainchain: it is logged, and tried
        // again with the next block.
        try {
            // Without the witness form: a blind bundle has no witness, and the mainchain reads it so (a
            // chain with its own extended formats, as zSide, could otherwise pick one the mainchain cannot read).
            DataStream blind{};
            blind << TX_NO_WITNESS(CTransaction{*tx});
            m_client.Call("receivewithdrawalbundle", Params({slot, HexStr(blind)}));
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
    vouch(hash);
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
    int failures{0};
    while (!m_stop) {
        std::string error;
        if (Sync(error)) {
            failures = 0;
            Mine();
        } else {
            ++failures;
        }
        // After failures, longer and longer waits: a mainchain node that answers wrong is not asked
        // the same again and again.
        const auto wait{failures <= 1 ? std::chrono::duration_cast<std::chrono::seconds>(POLL_INTERVAL) :
                                        std::min<std::chrono::seconds>(std::chrono::seconds{int64_t{1} << std::min(failures - 1, 4)}, MAX_RETRY_INTERVAL)};
        WAIT_LOCK(m_mutex, lock);
        m_wake.wait_for(lock, wait, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_stop.load() || m_woken; });
        m_woken = false;
    }
}

} // namespace sidechain
