// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/mainchain.h>

#include <logging.h>

#include <set>

namespace sidechain {
namespace {
constexpr uint8_t DB_BLOCK{'b'};
//! Version of the record: 2 since blocks keep the bundles they proposed.
constexpr uint8_t DB_VERSION{'v'};
constexpr uint32_t RECORD_VERSION{2};
//! The sidechain found in the slot (SlotIdentity).
constexpr uint8_t DB_SLOT_IDENTITY{'i'};
//! Set while blocks connected before the backfill are to be checked again.
constexpr uint8_t DB_RECHECK{'r'};
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
    for (it->Seek(BlockKey{0}); it->Valid(); it->Next()) {
        BlockKey key;
        MainBlock block;
        if (!it->GetKey(key) || key.height != m_blocks.size() || !it->GetValue(block)) break;
        // The record is only as long as it is consistent.
        if (!m_blocks.empty() && block.prev_hash != m_blocks.back().hash) break;
        if (block.bmm) m_bmm.emplace(*block.bmm, m_blocks.size());
        for (const MainBundleEvent& event : block.bundles) m_closed.emplace(event.hash, m_blocks.size());
        for (const uint256& hash : block.proposed) m_proposed.emplace(hash, m_blocks.size());
        m_blocks.push_back(std::move(block));
    }
    if (SlotIdentity identity; m_db->Read(DB_SLOT_IDENTITY, identity)) m_slot_identity = identity;
    m_recheck = m_db->Exists(DB_RECHECK);
    {
        const std::unique_ptr<CDBIterator> failures{m_db->NewIterator()};
        for (failures->Seek(std::make_pair(DB_FAILURE, uint256{})); failures->Valid(); failures->Next()) {
            std::pair<uint8_t, uint256> key;
            uint8_t failure;
            if (!failures->GetKey(key) || key.first != DB_FAILURE || !failures->GetValue(failure)) break;
            m_failures.emplace(key.second, static_cast<Failure>(failure));
        }
    }
    uint32_t version{0};
    if (!m_db->Read(DB_VERSION, version) || version < RECORD_VERSION) {
        if (m_blocks.empty()) {
            m_db->Write(DB_VERSION, RECORD_VERSION);
        } else {
            m_needs_backfill = true;
        }
    }
    LogInfo("Loaded the record of %d mainchain blocks%s", m_blocks.size(), m_needs_backfill ? ", whose proposed bundles are to be filled in" : "");
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
    if (it == m_closed.end()) return std::nullopt;
    return it->second;
}

void Mainchain::UpdatePending() const
{
    AssertLockHeld(m_mutex);
    // Each block changes the count by the bundles it proposed first, less those it closed first that
    // a block before it proposed first.
    for (size_t h{m_pending.size()}; h < m_blocks.size(); ++h) {
        int64_t count{h == 0 ? 0 : int64_t{m_pending[h - 1]}};
        const int height{static_cast<int>(h)};
        std::set<uint256> seen;
        for (const uint256& hash : m_blocks[h].proposed) {
            const auto first{m_proposed.find(hash)};
            if (!seen.insert(hash).second || first == m_proposed.end() || first->second != height) continue;
            const auto closed{m_closed.find(hash)};
            if (closed == m_closed.end() || closed->second > height) ++count;
        }
        seen.clear();
        for (const MainBundleEvent& event : m_blocks[h].bundles) {
            const auto first{m_closed.find(event.hash)};
            if (!seen.insert(event.hash).second || first == m_closed.end() || first->second != height) continue;
            const auto proposed{m_proposed.find(event.hash)};
            if (proposed != m_proposed.end() && proposed->second < height) --count;
        }
        m_pending.push_back(static_cast<uint32_t>(std::max<int64_t>(count, 0)));
    }
}

bool Mainchain::BundlePending(int main_height) const
{
    LOCK(m_mutex);
    if (main_height < 0 || m_blocks.empty()) return false;
    UpdatePending();
    return m_pending[std::min<size_t>(main_height, m_blocks.size() - 1)] > 0;
}

std::optional<uint256> Mainchain::BmmAt(int height) const
{
    LOCK(m_mutex);
    if (height < 0 || height >= static_cast<int>(m_blocks.size())) return std::nullopt;
    return m_blocks[height].bmm;
}

bool Mainchain::NeedsBackfill() const
{
    LOCK(m_mutex);
    return m_needs_backfill;
}

bool Mainchain::Backfill(int height, const uint256& hash, const std::vector<uint256>& proposed)
{
    LOCK(m_mutex);
    if (height < 0 || height >= static_cast<int>(m_blocks.size()) || m_blocks[height].hash != hash) return false;
    MainBlock& block{m_blocks[height]};
    for (const uint256& old : block.proposed) {
        const auto it{m_proposed.find(old)};
        if (it != m_proposed.end() && it->second == height) m_proposed.erase(it);
    }
    block.proposed = proposed;
    for (const uint256& p : proposed) {
        // The first proposal counts, which a block filled in below a later one is.
        const auto [it, added]{m_proposed.emplace(p, height)};
        if (!added && it->second > height) {
            InvalidatePending(it->second);
            it->second = height;
        }
    }
    InvalidatePending(height);
    if (m_db) m_db->Write(BlockKey{static_cast<uint32_t>(height)}, block);
    return true;
}

void Mainchain::BackfillDone()
{
    LOCK(m_mutex);
    m_needs_backfill = false;
    m_recheck = true;
    if (m_db) {
        CDBBatch batch{*m_db};
        batch.Write(DB_VERSION, RECORD_VERSION);
        batch.Write(DB_RECHECK, uint8_t{1});
        m_db->WriteBatch(batch, /*fSync=*/true);
    }
}

bool Mainchain::RecheckPending() const
{
    LOCK(m_mutex);
    return m_recheck;
}

void Mainchain::RecheckDone()
{
    LOCK(m_mutex);
    m_recheck = false;
    if (m_db) m_db->Erase(DB_RECHECK, /*fSync=*/true);
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
    for (const MainBundleEvent& event : block.bundles) m_closed.emplace(event.hash, m_blocks.size());
    for (const uint256& hash : block.proposed) m_proposed.emplace(hash, m_blocks.size());
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
        for (const MainBundleEvent& event : m_blocks[i].bundles) {
            const auto it{m_closed.find(event.hash)};
            if (it != m_closed.end() && it->second == static_cast<int>(i)) m_closed.erase(it);
        }
        for (const uint256& hash : m_blocks[i].proposed) {
            const auto it{m_proposed.find(hash)};
            if (it != m_proposed.end() && it->second == static_cast<int>(i)) m_proposed.erase(it);
        }
        if (!m_blocks[i].bmm) continue;
        const auto it{m_bmm.find(*m_blocks[i].bmm)};
        if (it != m_bmm.end() && it->second == static_cast<int>(i)) m_bmm.erase(it);
    }
    m_blocks.resize(keep);
    InvalidatePending(static_cast<int>(keep));
    return removed;
}

} // namespace sidechain
