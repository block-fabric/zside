// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_EXTRA_H
#define BITCOIN_RPC_EXTRA_H

#include <functional>

class CRPCTable;

/**
 * RPC methods a program adds to those of the node (the daemon: the host of the pages over RPC that the Chains app
 * uses for its sidechains, webui/remotehost.h), without the node depending on them. Set before the node starts.
 */
namespace rpc_extra {
//! Called with the table once the node's own methods are in it (AppInitMain).
inline std::function<void(CRPCTable&)> g_register;
//! Called first when the node is interrupted (shutdown): what waits in those methods returns, what they hold goes.
inline std::function<void()> g_interrupt;
//! Called in Shutdown once the scheduler stopped, before the node's parts go: what those methods hold is freed.
inline std::function<void()> g_shutdown;
} // namespace rpc_extra

#endif // BITCOIN_RPC_EXTRA_H
