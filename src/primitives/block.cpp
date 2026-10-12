// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <tinyformat.h>

#include <atomic>
#include <memory>
#include <sstream>

namespace {
std::atomic<bool> g_block_extensions{false};
} // namespace

void SetBlockExtensions(bool enabled) { g_block_extensions.store(enabled, std::memory_order_relaxed); }
bool BlockExtensionsEnabled() { return g_block_extensions.load(std::memory_order_relaxed); }

uint256 CBlockHeader::GetHash() const
{
    HashWriter hasher{};
    hasher << nVersion << hashPrevBlock << hashMerkleRoot << nTime << nBits << nNonce;
    if (IsExtended()) hasher << ext.hashSections;
    return hasher.GetHash();
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
