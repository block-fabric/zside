// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/mainchain.h>

#include <logging.h>
#include <tinyformat.h>
#include <util/fs.h>

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>

namespace sidechain {
namespace {
constexpr uint8_t DB_BLOCK{'b'};
//! Version of the record: 3 since blocks keep the bundles they proposed and those pending after them
//! with their scores. An older record cannot be read.
constexpr uint8_t DB_VERSION{'v'};
constexpr uint32_t RECORD_VERSION{3};
//! The sidechain found in the slot (SlotIdentity).
constexpr uint8_t DB_SLOT_IDENTITY{'i'};
//! Why blocks of this chain failed (Mainchain::Failure), by hash.
constexpr uint8_t DB_FAILURE{'F'};

/** Key of a block: its height, big endian so that the blocks are stored in order. */
struct BlockKey {
    uint32_t height{0};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_BLOCK);
        ser_writedata32be(s, height);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (ser_readdata8(s) != DB_BLOCK) throw std::ios_base::failure("not a block key");
        height = ser_readdata32be(s);
    }
};
} // namespace

Mainchain::Mainchain(const std::optional<DBParams>& db_params)
{
    if (!db_params) return;
    m_db = std::make_unique<CDBWrapper>(*db_params);
    const std::unique_ptr<CDBIterator> it{m_db->NewIterator()};
    if (uint32_t version{0}; !m_db->Read(DB_VERSION, version) || version < RECORD_VERSION) {
        it->Seek(BlockKey{0});
        if (BlockKey key; it->Valid() && it->GetKey(key)) {
            throw std::runtime_error(strprintf("The record of the mainchain in %s was written by an older release, which kept less of each block. "
                                               "Start this release with a new data directory.", fs::PathToString(db_params->path)));
        }
        m_db->Write(DB_VERSION, RECORD_VERSION);
    }
    for (it->Seek(BlockKey{0}); it->Valid(); it->Next()) {
        BlockKey key;
        MainBlock block;
        if (!it->GetKey(key) || key.height != m_blocks.size() || !it->GetValue(block)) break;
        // The record is only as long as it is consistent.
        if (!m_blocks.empty() && block.prev_hash != m_blocks.back().hash) break;
        if (block.bmm) m_bmm.emplace(*block.bmm, m_blocks.size());
        IndexEvents(block, m_blocks.size(), /*add=*/true);
        m_blocks.push_back(std::move(block));
    }
    if (SlotIdentity identity; m_db->Read(DB_SLOT_IDENTITY, identity)) m_slot_identity = identity;
    {
        const std::unique_ptr<CDBIterator> failures{m_db->NewIterator()};
        for (failures->Seek(std::make_pair(DB_FAILURE, uint256{})); failures->Valid(); failures->Next()) {
            std::pair<uint8_t, uint256> key;
            uint8_t failure;
            if (!failures->GetKey(key) || key.first != DB_FAILURE || !failures->GetValue(failure)) break;
            m_failures.emplace(key.second, static_cast<Failure>(failure));
        }
    }
    LogInfo("Loaded the record of %d mainchain blocks", m_blocks.size());
}

int Mainchain::Height() const
{
    LOCK(m_mutex);
    return static_cast<int>(m_blocks.size()) - 1;
}

uint256 Mainchain::TipHash() const
{
    LOCK(m_mutex);
    return m_blocks.empty() ? uint256{} : m_blocks.back().hash;
}

std::optional<MainBlock> Mainchain::GetBlock(int height) const
{
    LOCK(m_mutex);
    if (height < 0 || height >= static_cast<int>(m_blocks.size())) return std::nullopt;
    return m_blocks[height];
}

std::optional<int> Mainchain::CommittedHeight(const uint256& side_hash) const
{
    LOCK(m_mutex);
    const auto it{m_bmm.find(side_hash)};
    if (it != m_bmm.end()) return it->second;
    return std::nullopt;
}

std::optional<int> Mainchain::BmmHeight(const uint256& side_hash) const
{
    LOCK(m_mutex);
    const auto it{m_bmm.find(side_hash)};
    if (it != m_bmm.end()) return it->second;
    return m_assumed_height;
}

std::optional<int> Mainchain::ClosedHeight(const uint256& hash) const
{
    LOCK(m_mutex);
    const auto it{m_closed.find(hash)};
    if (it == m_closed.end() || it->second.empty()) return std::nullopt;
    return *it->second.begin();
}

bool Mainchain::ProposedBetween(const uint256& hash, int from, int to) const
{
    LOCK(m_mutex);
    const auto it{m_proposed.find(hash)};
    if (it == m_proposed.end()) return false;
    const auto at{it->second.lower_bound(from)};
    return at != it->second.end() && *at <= to;
}

bool Mainchain::ProposedSince(const uint256& hash, int committed, int to) const
{
    LOCK(m_mutex);
    // Pending after the block before the commitment: proposed before it, and not closed since.
    if (PendingAfter(hash, committed - 1)) return true;
    const auto it{m_proposed.find(hash)};
    if (it == m_proposed.end()) return false;
    const auto at{it->second.lower_bound(committed)};
    return at != it->second.end() && *at <= to;
}

void Mainchain::IndexEvents(const MainBlock& block, int height, bool add)
{
    AssertLockHeld(m_mutex);
    const auto index{[&](std::map<uint256, std::set<int>>& map, const uint256& hash) {
        if (add) {
            map[hash].insert(height);
            return;
        }
        const auto it{map.find(hash)};
        if (it == map.end()) return;
        it->second.erase(height);
        if (it->second.empty()) map.erase(it);
    }};
    for (const MainBundleEvent& event : block.bundles) index(m_closed, event.hash);
    for (const uint256& hash : block.proposed) index(m_proposed, hash);
}

bool Mainchain::PendingAfter(const uint256& hash, int height) const
{
    AssertLockHeld(m_mutex);
    // The last proposal at or below the height, and the last close: pending if the proposal is the
    // later one, or in the same block (closes come first).
    const auto last{[&](const std::map<uint256, std::set<int>>& map) -> std::optional<int> {
        const auto it{map.find(hash)};
        if (it == map.end()) return std::nullopt;
        const auto above{it->second.upper_bound(height)};
        if (above == it->second.begin()) return std::nullopt;
        return *std::prev(above);
    }};
    const auto proposed{last(m_proposed)};
    if (!proposed) return false;
    const auto closed{last(m_closed)};
    return !closed || *proposed >= *closed;
}

bool Mainchain::SupportedPending(int main_height, uint32_t min_score, const uint256& ours) const
{
    LOCK(m_mutex);
    if (main_height < 0 || m_blocks.empty()) return false;
    const MainBlock& block{m_blocks[std::min<size_t>(main_height, m_blocks.size() - 1)]};
    return std::any_of(block.pending.begin(), block.pending.end(), [&](const MainPendingBundle& bundle) {
        return bundle.hash != ours && bundle.score >= min_score;
    });
}

bool Mainchain::RisingLeader(int main_height, int window, uint32_t min_rise, const uint256& ours) const
{
    LOCK(m_mutex);
    if (main_height < 0 || m_blocks.empty()) return false;
    const int at{std::min(main_height, static_cast<int>(m_blocks.size()) - 1)};
    const std::vector<MainPendingBundle>& pending{m_blocks[at].pending};
    // The leader: the one score above all others. A tie leads nobody (as LEADING_BY_50 sees it).
    const MainPendingBundle* leader{nullptr};
    bool tie{false};
    for (const MainPendingBundle& bundle : pending) {
        if (!leader || bundle.score > leader->score) {
            leader = &bundle;
            tie = false;
        } else if (bundle.score == leader->score) {
            tie = true;
        }
    }
    if (!leader || tie || leader->hash == ours) return false;
    // Its score `window` blocks before (or after the first block on record, if fewer); a bundle
    // proposed since started at NEW_BUNDLE_SCORE (1).
    uint32_t before{1};
    for (const MainPendingBundle& bundle : m_blocks[std::max(at - window, 0)].pending) {
        if (bundle.hash == leader->hash) before = bundle.score;
    }
    return leader->score >= before + min_rise;
}

std::optional<uint256> Mainchain::BmmAt(int height) const
{
    LOCK(m_mutex);
    if (height < 0 || height >= static_cast<int>(m_blocks.size())) return std::nullopt;
    return m_blocks[height].bmm;
}

void Mainchain::NoteFailure(const uint256& block_hash, Failure failure)
{
    LOCK(m_mutex);
    m_failures[block_hash] = failure;
    // Before the block is marked failed, which the block index writes later: a node that stops in
    // between finds the reason, and takes the failure back if it was the record's.
    if (m_db) m_db->Write(std::make_pair(DB_FAILURE, block_hash), static_cast<uint8_t>(failure), /*fSync=*/true);
}

std::optional<Mainchain::Failure> Mainchain::GetFailure(const uint256& block_hash) const
{
    LOCK(m_mutex);
    const auto it{m_failures.find(block_hash)};
    if (it == m_failures.end()) return std::nullopt;
    return it->second;
}

void Mainchain::ForgetFailure(const uint256& block_hash)
{
    LOCK(m_mutex);
    if (m_failures.erase(block_hash) && m_db) m_db->Erase(std::make_pair(DB_FAILURE, block_hash));
}

std::optional<SlotIdentity> Mainchain::GetSlotIdentity() const
{
    LOCK(m_mutex);
    return m_slot_identity;
}

void Mainchain::SetSlotIdentity(const SlotIdentity& identity)
{
    LOCK(m_mutex);
    m_slot_identity = identity;
    if (m_db) m_db->Write(DB_SLOT_IDENTITY, identity, /*fSync=*/true);
}

bool Mainchain::Append(const MainBlock& block)
{
    LOCK(m_mutex);
    if (!m_blocks.empty() && block.prev_hash != m_blocks.back().hash) return false;
    // A sidechain block has one place in the mainchain; a second commitment to it means nothing.
    if (block.bmm) m_bmm.emplace(*block.bmm, m_blocks.size());
    IndexEvents(block, m_blocks.size(), /*add=*/true);
    if (m_db) m_db->Write(BlockKey{static_cast<uint32_t>(m_blocks.size())}, block);
    m_blocks.push_back(block);
    return true;
}

std::vector<MainBlock> Mainchain::Truncate(int height)
{
    LOCK(m_mutex);
    std::vector<MainBlock> removed;
    const size_t keep{height < 0 ? 0 : static_cast<size_t>(height) + 1};
    if (keep >= m_blocks.size()) return removed;
    removed.assign(m_blocks.begin() + keep, m_blocks.end());
    if (m_db) {
        CDBBatch batch{*m_db};
        for (size_t i{keep}; i < m_blocks.size(); ++i) batch.Erase(BlockKey{static_cast<uint32_t>(i)});
        m_db->WriteBatch(batch, /*fSync=*/true);
    }
    for (size_t i{keep}; i < m_blocks.size(); ++i) {
        IndexEvents(m_blocks[i], i, /*add=*/false);
        if (!m_blocks[i].bmm) continue;
        const auto it{m_bmm.find(*m_blocks[i].bmm)};
        if (it != m_bmm.end() && it->second == static_cast<int>(i)) m_bmm.erase(it);
    }
    m_blocks.resize(keep);
    return removed;
}

} // namespace sidechain
