// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZSIDE_MARKER_H
#define BITCOIN_ZSIDE_MARKER_H

#include <primitives/transaction.h>
#include <script/script.h>

#include <algorithm>

/** What every part of the node can tell about a shielded transaction without reading its bundle. */
namespace zside {

inline constexpr unsigned char SHIELDED_TAG[4]{0x5A, 0x4F, 0x52, 0x43}; // "ZORC"

/** Whether a script claims to be a shielded marker, well formed or not. */
inline bool IsShieldedMarker(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return false;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4) return false;
    return data.size() >= sizeof(SHIELDED_TAG) && std::equal(std::begin(SHIELDED_TAG), std::end(SHIELDED_TAG), data.begin());
}

/** Whether a transaction has a shielded marker: then it may have no transparent input. */
inline bool HasShieldedMarker(const CTransaction& tx)
{
    return std::any_of(tx.vout.begin(), tx.vout.end(), [](const CTxOut& out) { return IsShieldedMarker(out.scriptPubKey); });
}

} // namespace zside

#endif // BITCOIN_ZSIDE_MARKER_H
