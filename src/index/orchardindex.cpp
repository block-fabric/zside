// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/orchardindex.h>

#include <common/args.h>
#include <dbwrapper.h>
#include <interfaces/chain.h>
#include <primitives/block.h>
#include <sync.h>
#include <util/fs.h>
#include <util/log.h>
#include <zside/orchard.h>
#include <zside/shielded.h>

#include <map>

std::unique_ptr<OrchardIndex> g_orchardindex;

namespace {
constexpr uint8_t DB_SIZE{'s'};
//! (level, index) -> the node: a leaf at level 0, the root at level 32.
constexpr uint8_t DB_NODE{'n'};
//! The commitment of a note -> its position.
constexpr uint8_t DB_POSITION{'p'};
//! A block -> the size of the tree before it.
constexpr uint8_t DB_BLOCK{'b'};

struct NodeKey {
    uint8_t level;
    uint64_t index;
    SERIALIZE_METHODS(NodeKey, obj) { READWRITE(obj.level, obj.index); }
    friend auto operator<=>(const NodeKey&, const NodeKey&) = default;
};
} // namespace

class OrchardIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory, bool f_wipe)
        : BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "orchard", n_cache_size, f_memory, f_wipe) {}

    Mutex m_mutex;

    uint64_t Size() const
    {
        uint64_t size{0};
        Read(DB_SIZE, size);
        return size;
    }

    /** A node of the tree of `size` leaves: empty where the tree has no leaves yet. */
    uint256 Node(const std::map<NodeKey, uint256>& pending, unsigned level, uint64_t index, uint64_t size) const
    {
        if (level < 64 && (index << level) >= size) return zside::EmptyRoot(level);
        if (const auto it{pending.find({static_cast<uint8_t>(level), index})}; it != pending.end()) return it->second;
        uint256 node;
        if (!Read(std::make_pair(DB_NODE, NodeKey{static_cast<uint8_t>(level), index}), node)) return zside::EmptyRoot(level);
        return node;
    }

    /**
     * A node of the tree as it was with `size` leaves, `size` at most the current size: the current
     * node where its leaves were all in it already, computed again from its children where it was
     * cut (one per level at most, on the right edge).
     */
    uint256 NodeAt(unsigned level, uint64_t index, uint64_t size, uint64_t current) const
    {
        const std::map<NodeKey, uint256> none;
        const uint64_t first{index << level};
        if (first >= size) return zside::EmptyRoot(level);
        if (size == current || level == 0 || first + (uint64_t{1} << level) <= size) return Node(none, level, index, current);
        return zside::Combine(level - 1, NodeAt(level - 1, 2 * index, size, current), NodeAt(level - 1, 2 * index + 1, size, current));
    }

    /** Compute the nodes above the leaf at `position` again, in a tree of `size` leaves. */
    void Rehash(std::map<NodeKey, uint256>& pending, uint64_t position, uint64_t size) const
    {
        uint64_t index{position};
        for (unsigned level{0}; level < zside::TREE_DEPTH; ++level) {
            const uint64_t left{index & ~uint64_t{1}};
            pending[{static_cast<uint8_t>(level + 1), index >> 1}] =
                zside::Combine(level, Node(pending, level, left, size), Node(pending, level, left + 1, size));
            index >>= 1;
        }
    }

    void Flush(const std::map<NodeKey, uint256>& pending, CDBBatch& batch) const
    {
        for (const auto& [key, node] : pending) batch.Write(std::make_pair(DB_NODE, key), node);
    }
};

OrchardIndex::OrchardIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "orchardindex", "orchardidx"), m_db(std::make_unique<OrchardIndex::DB>(n_cache_size, f_memory, f_wipe)) {}

OrchardIndex::~OrchardIndex() = default;

BaseIndex::DB& OrchardIndex::GetDB() const { return *m_db; }

bool OrchardIndex::CustomInit(const std::optional<interfaces::BlockRef>& block)
{
    // From the start: the tree is empty, whatever an earlier one left (it is overwritten as it grows).
    if (!block) {
        LOCK(m_db->m_mutex);
        CDBBatch batch{*m_db};
        batch.Write(DB_SIZE, uint64_t{0});
        m_db->WriteBatch(batch);
    }
    return true;
}

bool OrchardIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    assert(block.data);
    LOCK(m_db->m_mutex);
    // A block may come again (a reconnection, a replay after an unclean shutdown): its notes go
    // where they went the first time.
    uint64_t size{m_db->Size()};
    if (uint64_t before; m_db->Read(std::make_pair(DB_BLOCK, block.hash), before)) size = before;
    CDBBatch batch{*m_db};
    batch.Write(std::make_pair(DB_BLOCK, block.hash), size);
    std::map<NodeKey, uint256> pending;
    for (const CTransactionRef& tx : block.data->vtx) {
        // As consensus: the coinbase has no bundle.
        if (tx->IsCoinBase() || !zside::HasShieldedMarker(*tx)) continue;
        std::string error;
        const auto bundle{zside::Bundle::FromTx(*tx, error)};
        // The block is valid: its bundles are well formed.
        if (!bundle) continue;
        for (const uint256& cmx : bundle->Commitments()) {
            pending[{0, size}] = cmx;
            batch.Write(std::make_pair(DB_POSITION, cmx), size);
            ++size;
            m_db->Rehash(pending, size - 1, size);
        }
    }
    m_db->Flush(pending, batch);
    batch.Write(DB_SIZE, size);
    m_db->WriteBatch(batch);
    return true;
}

bool OrchardIndex::CustomRemove(const interfaces::BlockInfo& block)
{
    LOCK(m_db->m_mutex);
    uint64_t before;
    if (!m_db->Read(std::make_pair(DB_BLOCK, block.hash), before)) {
        // A block the index never took in (one under a UTXO snapshot): nothing of it to remove.
        LogWarning("orchardindex: no record of block %s to remove", block.hash.ToString());
        return true;
    }
    // The tree as it was: the leaves beyond its size are ignored from now, and its right edge
    // is computed again without them.
    CDBBatch batch{*m_db};
    std::map<NodeKey, uint256> pending;
    if (before > 0) m_db->Rehash(pending, before - 1, before);
    m_db->Flush(pending, batch);
    batch.Write(DB_SIZE, before);
    batch.Erase(std::make_pair(DB_BLOCK, block.hash));
    m_db->WriteBatch(batch);
    return true;
}

std::optional<OrchardIndex::Witnesses> OrchardIndex::FindNotes(const std::vector<uint256>& cmxs, const std::optional<uint256>& next_block) const
{
    // Under one lock: the paths all lead to the same root.
    LOCK(m_db->m_mutex);
    uint64_t size{m_db->Size()};
    if (next_block) {
        uint64_t before;
        if (!m_db->Read(std::make_pair(DB_BLOCK, *next_block), before) || before > size) return std::nullopt;
        size = before;
    }
    Witnesses out;
    out.anchor = m_db->NodeAt(zside::TREE_DEPTH, 0, size, m_db->Size());
    for (const uint256& cmx : cmxs) {
        auto witness{FindNoteLocked(cmx, size)};
        if (!witness) return std::nullopt;
        out.paths.push_back(std::move(*witness));
    }
    return out;
}

std::optional<OrchardWitness> OrchardIndex::FindNoteLocked(const uint256& cmx, uint64_t size) const
{
    uint64_t position;
    if (!m_db->Read(std::make_pair(DB_POSITION, cmx), position)) return std::nullopt;
    const uint64_t current{m_db->Size()};
    // A note of a block the index has removed since, or after the tree asked for.
    if (position >= size || size > current) return std::nullopt;
    const std::map<NodeKey, uint256> none;
    if (m_db->Node(none, 0, position, current) != cmx) return std::nullopt;
    OrchardWitness witness;
    witness.position = position;
    for (unsigned level{0}; level < zside::TREE_DEPTH; ++level) {
        witness.path[level] = m_db->NodeAt(level, (position >> level) ^ 1, size, current);
    }
    witness.anchor = m_db->NodeAt(zside::TREE_DEPTH, 0, size, current);
    return witness;
}

std::pair<uint64_t, uint256> OrchardIndex::Tree() const
{
    LOCK(m_db->m_mutex);
    const uint64_t size{m_db->Size()};
    const std::map<NodeKey, uint256> none;
    return {size, size == 0 ? zside::EmptyRoot(zside::TREE_DEPTH) : m_db->Node(none, zside::TREE_DEPTH, 0, size)};
}
