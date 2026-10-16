// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of zSide (src/webui/app/src/chain/local.ts): the RPC methods of the shielded
// pool and the shielded wallet they call (src/rpc/shielded.cpp, src/wallet/rpc/shielded.cpp), and the one
// host method they need: the viewing key, shown only after the desktop asked (zside.viewingkey.export).

#include <webui/local.h>

#include <interfaces/node.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>

namespace webui::local {

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // The pool of the chain: read only.
        {"getshieldedinfo", Bridge::Thread::WORKER},
        // What the shielded account of the wallet holds and did: read only.
        {"getshieldedbalance", Bridge::Thread::WORKER},
        {"listshieldednotes", Bridge::Thread::WORKER},
        {"listshieldedtransactions", Bridge::Thread::WORKER},
        {"listmeltcast", Bridge::Thread::WORKER},
        // An address of the account (a new one looks unrelated to the others); the first call of a locked
        // wallet whose account is not set up yet sets it up with the keys.
        {"getshieldedaddress", Bridge::Thread::WORKER},
        // Not exportshieldedviewingkey: the node answers it even with the wallet locked, and the key shows
        // every private payment of the account. The page asks for it through zside.viewingkey.export, which
        // the desktop confirms first (audit port-2 #3).
        // A viewing key watched in a wallet without keys: the blocks are scanned again, which takes long.
        {"importshieldedviewingkey", Bridge::Thread::LONG},
        // Payments out of the pool and into it: each proves a bundle (Halo 2, seconds), on the long lane
        // so that the quick calls of the pages never wait behind a proof.
        {"sendshielded", Bridge::Thread::LONG},
        {"shieldcoins", Bridge::Thread::LONG},
        {"unshieldcoins", Bridge::Thread::LONG},
        {"bumpshieldedfee", Bridge::Thread::LONG},
        // Melt & cast: plans the wallet carries out as the blocks come.
        {"meltcoins", Bridge::Thread::WORKER},
        {"castcoins", Bridge::Thread::WORKER},
        {"cancelmeltcast", Bridge::Thread::WORKER},
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // Each spends with the keys of the account (or locks the coins it plans to melt); getshieldedaddress sets
    // the account up from the keys the first time.
    return {"sendshielded", "shieldcoins", "unshieldcoins", "bumpshieldedfee", "meltcoins", "getshieldedaddress"};
}

Window::ConfirmDialog ViewingKeyConfirmation(const std::string& wallet)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Show the viewing key";
    dialog.message = strprintf("Wallet: %s\n\n"
                               "zSide asks to show the viewing key of the shielded account of this wallet. Whoever has it sees "
                               "every payment the account receives and makes, past and future, with the amounts and the memos. "
                               "It cannot spend, and it cannot be taken back once given.\n\nShow it only if you asked for it.",
                               guard::OneLine(wallet, 64));
    dialog.accept = "Show the key";
    return dialog;
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm)
{
    // zside.viewingkey.export {wallet}: the viewing key of the account of a wallet, after the user said yes on
    // the desktop (outside the page); a no answers BRIDGE_CANCELLED and the node is not asked.
    bridge.RegisterHost("zside.viewingkey.export", [node, confirm](const UniValue& params) -> UniValue {
        const UniValue& wallet{params.find_value("wallet")};
        if (!wallet.isStr() || wallet.get_str().size() > 1024) throw HostError(BRIDGE_INVALID_REQUEST, "wallet: the name of a wallet");
        if (!confirm || !confirm(ViewingKeyConfirmation(wallet.get_str()))) {
            throw HostError(BRIDGE_CANCELLED, "The viewing key was not shown: it was declined.");
        }
        return node().executeRpc("exportshieldedviewingkey", UniValue{UniValue::VARR}, WalletUri(wallet.get_str()));
    });
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local
