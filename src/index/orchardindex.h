// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_ORCHARDINDEX_H
#define BITCOIN_INDEX_ORCHARDINDEX_H

#include <index/base.h>
#include <interfaces/types.h>
#include <uint256.h>
#include <zside/witness.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace interfaces {
class Chain;
}

/** Wallets need the index to spend shielded notes: on unless turned off. */
inline constexpr bool DEFAULT_ORCHARDINDEX{true};

/**
 * zSide: the whole note commitment tree of the shielded pool, node by node, so that a wallet can
 * prove its notes are in it (consensus keeps only the right edge). A note is found by its
 * commitment. The tree is that of the chain up to the block the index has reached.
 */
class OrchardIndex final : public BaseIndex
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;

    //! Blocks once read are not needed again (a removal needs only the record of the block).
    bool AllowPrune() const override { return true; }

    bool RebuildWhenLost() const override { return true; }

protected:
    bool CustomInit(const std::optional<interfaces::BlockRef>& block) override;
    bool CustomAppend(const interfaces::BlockInfo& block) override;
    bool CustomRemove(const interfaces::BlockInfo& block) override;
    BaseIndex::DB& GetDB() const override;

    std::optional<OrchardWitness> FindNoteLocked(const uint256& cmx, uint64_t size) const;

public:
    explicit OrchardIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory = false, bool f_wipe = false);
    ~OrchardIndex() override;

    /**
     * Where several notes are, with their paths to the same root: that of the tree as it was before
     * the block `next_block` (the one the block before it left), or of the current tree. Nullopt if
     * one is not in that tree, or the index has no record of the block (not on its chain).
     */
    using Witnesses = OrchardWitnesses;
    std::optional<Witnesses> FindNotes(const std::vector<uint256>& cmxs, const std::optional<uint256>& next_block = std::nullopt) const;
    /** How many notes the tree holds, and its root. */
    std::pair<uint64_t, uint256> Tree() const;
};

extern std::unique_ptr<OrchardIndex> g_orchardindex;

#endif // BITCOIN_INDEX_ORCHARDINDEX_H
