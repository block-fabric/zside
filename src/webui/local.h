// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_LOCAL_H
#define BITCOIN_WEBUI_LOCAL_H

#include <webui/bridge.h>
#include <webui/confirm.h>
#include <webui/guard.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class uint256;
namespace interfaces {
class Chain;
class Node;
} // namespace interfaces

/**
 * What a chain adds to the host for its own pages (the pages of src/webui/app/src/chain/local.ts), in
 * local.cpp, the one host file a chain made from the template changes: the kit itself stays as it is, so
 * merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
 */
namespace webui::local {

/** RPC methods the chain's pages call, added to the page's allowlist (rpcpolicy.cpp): what reads. */
std::map<std::string, Bridge::Thread> RpcMethods();

/**
 * Of these, the wallet methods that need the keys to read (or to make a key), not to spend: a locked wallet answers -13,
 * and the page asks the host (rpc.confirmed), which asks for the passphrase in its own dialog (confirm.h, KEYS).
 */
std::set<std::string> KeyedRpcMethods();

/**
 * The RPC methods of the chain's pages that pay, sign or change what the node does (a registration, a trade, a vote, a
 * bond): never in the page's allowlist. The page calls them through the host (rpc.confirmed; the kit's walletRpc does), which
 * describes each from its parameters (the describer, confirm::Generic does most: destinations first and whole, then amounts),
 * asks the user in the dialog window of the host with the passphrase of a locked wallet typed there, and only then runs it
 * with the keys unlocked for it alone (audit 7 #1). confirm::Method::Kind SPEND for a wallet's, NODE for the node's.
 */
std::map<std::string, confirm::Method> ConfirmedRpcMethods();

/** The node, while it runs (throws a HostError before); the host methods take it per call. */
using NodeFn = std::function<interfaces::Node&()>;

/**
 * Host methods of the chain's pages ("<chain>.something"), registered after the kit's. `confirm` asks the
 * user in the dialog window of the host (App::Confirm, dialog.h: in the look of the app, outside the page): what a
 * page must not do on its own word, such as showing a key, asks there first and answers BRIDGE_CANCELLED on a no
 * (audit port-2 #3). A Window::ConfirmDialog is all it takes (its level may say DANGER). `keys` runs a call with
 * the keys of a wallet, the passphrase of a locked one typed in that dialog, never given by the page (audit 7 #1):
 * keys(wallet, dialog, always, fee_unknown, run), as rpc.confirmed does (confirm.h KeysFn); empty without wallets.
 */
void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys);

/** zSide: what the desktop asks before the viewing key of a wallet is shown (zside.viewingkey.export). */
Window::ConfirmDialog ViewingKeyConfirmation(const std::string& wallet);

/**
 * What each output of the coinbase of a block of the wallet pays, by the chain's own words (a short id,
 * "" for an output it has nothing to say of): the history shows such an output by its role rather than
 * "Mined" (chain/roles.ts, COINBASE_ROLES). The template has none: empty.
 */
std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block);

} // namespace webui::local

#endif // BITCOIN_WEBUI_LOCAL_H
