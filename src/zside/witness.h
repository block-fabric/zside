// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZSIDE_WITNESS_H
#define BITCOIN_ZSIDE_WITNESS_H

#include <uint256.h>

#include <array>
#include <cstdint>
#include <vector>

/** Where a note is in the tree, and the path that proves it, as of the root `anchor`. */
struct OrchardWitness {
    uint64_t position{0};
    std::array<uint256, 32> path;
    uint256 anchor;
};

/** Where several notes are, with their paths to the same root. */
struct OrchardWitnesses {
    uint256 anchor;
    std::vector<OrchardWitness> paths;
};

#endif // BITCOIN_ZSIDE_WITNESS_H
