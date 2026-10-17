// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_RPCPOLICY_H
#define BITCOIN_WEBUI_RPCPOLICY_H

#include <webui/bridge.h>

#include <optional>
#include <string>
#include <vector>

namespace webui {

/**
 * The RPC methods the pages call through the bridge (kind "rpc"), and where they run. The page
 * calls no other: whatever it would need beyond them is a host method with rules of its own (what spends, signs or
 * changes the votes of the node is confirmed on the desktop first: confirm.h, rpc.confirmed), and a command typed by the
 * user goes through the console (console.run), which runs any command, one that does more than read on the user's word
 * given to the desktop (guard.h). The policy holds in the
 * end-to-end tests too: their script sets the node up with "testrpc" (Bridge::OpenTestRpc).
 *
 * src/webui/app/src/test/rpcpolicy.test.ts checks that every method the pages call is listed here.
 */
std::optional<Bridge::Thread> PageRpcPolicy(const std::string& method);

/** The methods of PageRpcPolicy, in order (the tests). */
std::vector<std::string> PageRpcMethods();

/**
 * Of the methods of the page's policy, the wallet methods that need the keys of an encrypted wallet without spending (they
 * read with them, or make a key): a locked wallet answers -13, and the page then asks the host (rpc.confirmed), which asks
 * the user for the passphrase in its own dialog and runs the call with the keys unlocked for it alone. What spends or signs
 * is never in the policy: confirm.h.
 */
bool KeyedWalletRpc(const std::string& method);

} // namespace webui

#endif // BITCOIN_WEBUI_RPCPOLICY_H
