// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/mainchain.h>

#include <logging.h>

namespace sidechain {
namespace {
constexpr uint8_t DB_BLOCK{'b'};
//! Version of the record: 2 since blocks keep the bundles they proposed.
constexpr uint8_t DB_VERSION{'v'};
constexpr uint32_t RECORD_VERSION{2};

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

bool Mainchain::BundlePending(int main_height) const
{
    LOCK(m_mutex);
    for (const auto& [hash, proposed] : m_proposed) {
        if (proposed > main_height) continue;
        const auto closed{m_closed.find(hash)};
        if (closed == m_closed.end() || closed->second > main_height) return true;
    }
    return false;
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
    for (const uint256& p : proposed) m_proposed.emplace(p, height);
    if (m_db) m_db->Write(BlockKey{static_cast<uint32_t>(height)}, block);
    return true;
}

void Mainchain::BackfillDone()
{
    LOCK(m_mutex);
    m_needs_backfill = false;
    if (m_db) m_db->Write(DB_VERSION, RECORD_VERSION, /*fSync=*/true);
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
    return removed;
}

} // namespace sidechain
