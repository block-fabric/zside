// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_LOCAL_H
#define BITCOIN_WEBUI_LOCAL_H

#include <webui/bridge.h>
#include <webui/guard.h>

#include <functional>
#include <map>
#include <set>
#include <string>

namespace interfaces {
class Node;
} // namespace interfaces

/**
 * What a chain adds to the host for its own pages (the pages of src/webui/app/src/chain/local.ts), in
 * local.cpp, the one host file a chain made from the template changes: the kit itself stays as it is, so
 * merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
 */
namespace webui::local {

/** RPC methods the chain's pages call, added to the page's allowlist (rpcpolicy.cpp). */
std::map<std::string, Bridge::Thread> RpcMethods();

/** Of these, the wallet methods that need the keys (they go through wallet.withKeys on an encrypted wallet). */
std::set<std::string> KeyedRpcMethods();

/** The node, while it runs (throws a HostError before); the host methods take it per call. */
using NodeFn = std::function<interfaces::Node&()>;

/**
 * Host methods of the chain's pages ("<chain>.something"), registered after the kit's. `confirm` asks the
 * user on the desktop (Window::Confirm, outside the page): what a page must not do on its own word, such as
 * showing a key, asks there first and answers BRIDGE_CANCELLED on a no (audit port-2 #3).
 */
void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm);

} // namespace webui::local

#endif // BITCOIN_WEBUI_LOCAL_H
