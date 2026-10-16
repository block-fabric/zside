// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_SIDECHAINAPI_H
#define BITCOIN_WEBUI_SIDECHAINAPI_H

#include <consensus/amount.h>
#include <univalue.h>
#include <webui/guard.h>

#include <functional>
#include <set>
#include <string>

namespace interfaces {
class Node;
} // namespace interfaces

namespace webui {
class Bridge;

/**
 * The host methods of the pages of a sidechain (Peg, Merged mining, its dashboard) that need its
 * mainchain node, which the node follows (sidechain::Follower, -mainchainrpc*):
 *
 *   peg.connection                  how the node reaches the mainchain node: {connected, error, node, slot,
 *                                   wallet, walletSet, auth: "password" | "cookie", cookieFile}
 *   peg.call {method, params, wallet}  a method of the mainchain node, from a list that reads only (the
 *                                   drivechain state of this sidechain: bundles and their votes, deposits,
 *                                   the blocks that committed to it), and of its wallet (balances, a new address)
 *   peg.deposit {destination, amount}  the deposit helper: the wallet of the mainchain node deposits `amount`
 *                                   (coins of the mainchain, text) to this sidechain, to `destination`, a
 *                                   deposit address of this sidechain (its slot, a valid address of this chain)
 *   bmm.set {mine, address?, always?, amount?}  merged mining on (setbmm: the address of the fees; with
 *                                   `always`, `amount` offered for every block of the mainchain) or off
 *   bmm.request {address, amount}   one block mined by hand (requestbmmblock), `amount` offered for it
 *
 * What spends the wallet of the mainchain node (a deposit, merged mining turned on, a block asked for) waits for
 * the user's word, given in the dialog window of the host (App::Confirm), with the amount the host parsed first; an offer for
 * merged mining is at most MAX_BMM_OFFER (audit port-1 #4). setbmm and requestbmmblock are not methods the
 * page calls itself (rpcpolicy.cpp).
 *
 * The page never names the mainchain node or its credentials: the node's own connection is used.
 */
class SidechainApi
{
public:
    using NodeFn = std::function<interfaces::Node&()>;

    SidechainApi(NodeFn node, guard::ConfirmFn confirm);

    /** The most an offer of merged mining may be, for one block of the mainchain (0.01 coins). */
    static constexpr CAmount MAX_BMM_OFFER{1'000'000};

    void Register(Bridge& bridge);

    /** Run a method directly (the tests; Register uses the same functions). Throws HostError. */
    UniValue Call(const std::string& method, const UniValue& params);

    /** The methods of the mainchain node peg.call runs, and those of them that go to its wallet. */
    static const std::set<std::string>& MainchainMethods();
    static const std::set<std::string>& MainchainWalletMethods();

private:
    NodeFn m_node;
    guard::ConfirmFn m_confirm;
};

} // namespace webui

#endif // BITCOIN_WEBUI_SIDECHAINAPI_H
